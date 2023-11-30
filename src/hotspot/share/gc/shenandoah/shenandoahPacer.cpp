/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahPacer.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "runtime/atomic.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/threadSMR.hpp"

/*
 * In normal concurrent cycle, we have to pace the application to let GC finish.
 *
 * Here, we do not know how large would be the collection set, and what are the
 * relative performances of the each stage in the concurrent cycle, and so we have to
 * make some assumptions.
 *
 * For concurrent mark, there is no clear notion of progress. The moderately accurate
 * and easy to get metric is the amount of live objects the mark had encountered. But,
 * that does directly correlate with the used heap, because the heap might be fully
 * dead or fully alive. We cannot assume either of the extremes: we would either allow
 * application to run out of memory if we assume heap is fully dead but it is not, and,
 * conversely, we would pacify application excessively if we assume heap is fully alive
 * but it is not. So we need to guesstimate the particular expected value for heap liveness.
 * The best way to do this is apparently recording the past history.
 *
 * For concurrent evac and update-refs, we are walking the heap per-region, and so the
 * notion of progress is clear: we get reported the "used" size from the processed regions
 * and use the global heap-used as the baseline.
 *
 * The allocatable space when GC is running is "free" at the start of phase, but the
 * accounted budget is based on "used". So, we need to adjust the tax knowing that.
 */

void ShenandoahPacer::setup_for_mark() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t live = update_and_get_progress_history();
  size_t free = _heap->free_set()->available();

  size_t non_taxable = free * ShenandoahPacingCycleSlack / 100;
  size_t taxable = free - non_taxable;

  double tax = 1.0 * live / taxable; // base tax for available free space
  tax *= 1;                          // mark can succeed with immediate garbage, claim all available space
  tax *= ShenandoahPacingSurcharge;  // additional surcharge to help unclutter heap

  restart_with(non_taxable, tax);

  log_info(gc, ergo)("Pacer for Mark. Expected Live: " SIZE_FORMAT "%s, Free: " SIZE_FORMAT "%s, "
                     "Non-Taxable: " SIZE_FORMAT "%s, Alloc Tax Rate: %.1fx",
                     byte_size_in_proper_unit(live),        proper_unit_for_byte_size(live),
                     byte_size_in_proper_unit(free),        proper_unit_for_byte_size(free),
                     byte_size_in_proper_unit(non_taxable), proper_unit_for_byte_size(non_taxable),
                     tax);
}

void ShenandoahPacer::setup_for_evac() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t used = _heap->collection_set()->used();
  size_t free = _heap->free_set()->available();

  size_t non_taxable = free * ShenandoahPacingCycleSlack / 100;
  size_t taxable = free - non_taxable;

  double tax = 1.0 * used / taxable; // base tax for available free space
  tax *= 2;                          // evac is followed by update-refs, claim 1/2 of remaining free
  tax = MAX2<double>(1, tax);        // never allocate more than GC processes during the phase
  tax *= ShenandoahPacingSurcharge;  // additional surcharge to help unclutter heap

  restart_with(non_taxable, tax);

  log_info(gc, ergo)("Pacer for Evacuation. Used CSet: " SIZE_FORMAT "%s, Free: " SIZE_FORMAT "%s, "
                     "Non-Taxable: " SIZE_FORMAT "%s, Alloc Tax Rate: %.1fx",
                     byte_size_in_proper_unit(used),        proper_unit_for_byte_size(used),
                     byte_size_in_proper_unit(free),        proper_unit_for_byte_size(free),
                     byte_size_in_proper_unit(non_taxable), proper_unit_for_byte_size(non_taxable),
                     tax);
}

void ShenandoahPacer::setup_for_updaterefs() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t used = _heap->used();
  size_t free = _heap->free_set()->available();

  size_t non_taxable = free * ShenandoahPacingCycleSlack / 100;
  size_t taxable = free - non_taxable;

  double tax = 1.0 * used / taxable; // base tax for available free space
  tax *= 1;                          // update-refs is the last phase, claim the remaining free
  tax = MAX2<double>(1, tax);        // never allocate more than GC processes during the phase
  tax *= ShenandoahPacingSurcharge;  // additional surcharge to help unclutter heap

  restart_with(non_taxable, tax);

  log_info(gc, ergo)("Pacer for Update Refs. Used: " SIZE_FORMAT "%s, Free: " SIZE_FORMAT "%s, "
                     "Non-Taxable: " SIZE_FORMAT "%s, Alloc Tax Rate: %.1fx",
                     byte_size_in_proper_unit(used),        proper_unit_for_byte_size(used),
                     byte_size_in_proper_unit(free),        proper_unit_for_byte_size(free),
                     byte_size_in_proper_unit(non_taxable), proper_unit_for_byte_size(non_taxable),
                     tax);
}

/*
 * In idle phase, we have to pace the application to let control thread react with GC start.
 *
 * Here, we have rendezvous with concurrent thread that adds up the budget as it acknowledges
 * it had seen recent allocations. It will naturally pace the allocations if control thread is
 * not catching up. To bootstrap this feedback cycle, we need to start with some initial budget
 * for applications to allocate at.
 */

void ShenandoahPacer::setup_for_idle() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t initial = _heap->max_capacity() / 100 * ShenandoahPacingIdleSlack;
  double tax = 1;

  restart_with(initial, tax);

  log_info(gc, ergo)("Pacer for Idle. Initial: " SIZE_FORMAT "%s, Alloc Tax Rate: %.1fx",
                     byte_size_in_proper_unit(initial), proper_unit_for_byte_size(initial),
                     tax);
}

/*
 * There is no useful notion of progress for these operations. To avoid stalling
 * the allocators unnecessarily, allow them to run unimpeded.
 */

void ShenandoahPacer::setup_for_reset() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t initial = _heap->max_capacity();
  restart_with(initial, 1.0);

  log_info(gc, ergo)("Pacer for Reset. Non-Taxable: " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(initial), proper_unit_for_byte_size(initial));
}

size_t ShenandoahPacer::update_and_get_progress_history() {
  if (_progress == -1) {
    // First initialization, report some prior
    Atomic::store(&_progress, (intptr_t)PACING_PROGRESS_ZERO);
    return (size_t) (_heap->max_capacity() * 0.1);
  } else {
    // Record history, and reply historical data
    _progress_history->add(_progress);
    Atomic::store(&_progress, (intptr_t)PACING_PROGRESS_ZERO);
    return (size_t) (_progress_history->avg() * HeapWordSize);
  }
}

void ShenandoahPacer::restart_with(size_t non_taxable_bytes, double tax_rate) {
  size_t initial = (size_t)(non_taxable_bytes * tax_rate) >> LogHeapWordSize;
  STATIC_ASSERT(sizeof(size_t) <= sizeof(intptr_t));
  Atomic::xchg(&_budget, (intptr_t)initial, memory_order_relaxed);
  Atomic::store(&_tax_rate, tax_rate);
  Atomic::inc(&_epoch);

  // Shake up stalled waiters after budget update.
  _need_notify_waiters.try_set();
}

bool ShenandoahPacer::claim_for_alloc(size_t words, bool force) {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  intptr_t tax = MAX2<intptr_t>(1, words * Atomic::load(&_tax_rate));

  intptr_t cur = 0;
  intptr_t new_val = 0;
  do {
    cur = Atomic::load(&_budget);
    if (cur < tax && !force) {
      // Progress depleted, alas.
      return false;
    }
    new_val = cur - tax;
  } while (Atomic::cmpxchg(&_budget, cur, new_val, memory_order_relaxed) != cur);
  return true;
}

void ShenandoahPacer::unpace_for_alloc(intptr_t epoch, size_t words) {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  if (Atomic::load(&_epoch) != epoch) {
    // Stale ticket, no need to unpace.
    return;
  }

  size_t tax = MAX2<size_t>(1, words * Atomic::load(&_tax_rate));
  add_budget(tax);
}

intptr_t ShenandoahPacer::epoch() {
  return Atomic::load(&_epoch);
}

void ShenandoahPacer::pace_for_alloc(size_t words) {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  // Fast path: try to allocate right away
  bool claimed = claim_for_alloc(words, false);
  if (claimed) {
    return;
  }

  // Forcefully claim the budget: it may go negative at this point, and
  // GC should replenish for this and subsequent allocations. After this claim,
  // we would wait a bit until our claim is matched by additional progress,
  // or the time budget depletes.
  claimed = claim_for_alloc(words, true);
  assert(claimed, "Should always succeed");

  // Threads that are attaching should not block at all: they are not
  // fully initialized yet. Blocking them would be awkward.
  // This is probably the path that allocates the thread oop itself.
  //
  // Thread which is not an active Java thread should also not block.
  // This can happen during VM init when main thread is still not an
  // active Java thread.
  JavaThread* current = JavaThread::current();
  if (current->is_attaching_via_jni() ||
      !current->is_active_Java_thread()) {
    return;
  }

  double start = os::elapsedTime();

  size_t max_ms = ShenandoahPacingMaxDelay;
  size_t total_ms = 0;

  while (true) {
    // We could instead assist GC, but this would suffice for now.
    size_t cur_ms = (max_ms > total_ms) ? (max_ms - total_ms) : 1;
    wait(cur_ms);

    double end = os::elapsedTime();
    total_ms = (size_t)((end - start) * 1000);

    if (total_ms > max_ms || Atomic::load(&_budget) >= 0) {
      // Exiting if either:
      //  a) Spent local time budget to wait for enough GC progress.
      //     Breaking out and allocating anyway, which may mean we outpace GC,
      //     and start Degenerated GC cycle.
      //  b) The budget had been replenished, which means our claim is satisfied.
      ShenandoahThreadLocalData::add_paced_time(JavaThread::current(), end - start);
      break;
    }
  }
}

void ShenandoahPacer::wait(size_t time_ms) {
  // Perform timed wait. It works like like sleep(), except without modifying
  // the thread interruptible status. MonitorLocker also checks for safepoints.
  assert(time_ms > 0, "Should not call this with zero argument, as it would stall until notify");
  assert(time_ms <= LONG_MAX, "Sanity");
  MonitorLocker locker(_wait_monitor);
  _wait_monitor->wait((long)time_ms);
}

void ShenandoahPacer::notify_waiters() {
  if (_need_notify_waiters.try_unset()) {
    MonitorLocker locker(_wait_monitor);
    _wait_monitor->notify_all();
  }
}

void ShenandoahPacer::flush_stats_to_cycle() {
  double sum = 0;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    sum += ShenandoahThreadLocalData::paced_time(t);
  }
  ShenandoahHeap::heap()->phase_timings()->record_phase_time(ShenandoahPhaseTimings::pacing, sum);
}

void ShenandoahPacer::print_cycle_on(outputStream* out) {
  MutexLocker lock(Threads_lock);

  double now = os::elapsedTime();
  double total = now - _last_time;
  _last_time = now;

  out->cr();
  out->print_cr("Allocation pacing accrued:");

  size_t threads_total = 0;
  size_t threads_nz = 0;
  double sum = 0;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    double d = ShenandoahThreadLocalData::paced_time(t);
    if (d > 0) {
      threads_nz++;
      sum += d;
      out->print_cr("  %5.0f of %5.0f ms (%5.1f%%): %s",
              d * 1000, total * 1000, d/total*100, t->name());
    }
    threads_total++;
    ShenandoahThreadLocalData::reset_paced_time(t);
  }
  out->print_cr("  %5.0f of %5.0f ms (%5.1f%%): <total>",
          sum * 1000, total * 1000, sum/total*100);

  if (threads_total > 0) {
    out->print_cr("  %5.0f of %5.0f ms (%5.1f%%): <average total>",
            sum / threads_total * 1000, total * 1000, sum / threads_total / total * 100);
  }
  if (threads_nz > 0) {
    out->print_cr("  %5.0f of %5.0f ms (%5.1f%%): <average non-zero>",
            sum / threads_nz * 1000, total * 1000, sum / threads_nz / total * 100);
  }
  out->cr();
}

// A value of 48 signifies that updating a region is 48 times easier than evacuating a region, in proportion to the
// size of the relevant data.  The relevant data for evacuation is the amount of live data within the region.  The
// relevant data for evacuation is the also the live data within the region.  The value of the evacuate_vs_update_factor
// is adjusted dynamically.  If we find ourselves more likely to throttle during evac than update refs, we increase the
// value. If more likely to throttle during update refs than evacuate, decrease this number.  Note that evacauating has
// to read every value and write every value.  Updating only has to read reference values (N % of total live memory)
// and then it overwrites the same value in x% of cases (i.e. if the original value points to CSET).  Different workloads
// will behave differently.
const double ShenandoahThrottler::INITIAL_EVACUATE_VS_UPDATE_FACTOR = 48.0;

// A value of 32 signifies that it is 32 times easier to process a promote-in-place region than to evacuate the live
// data within a region.  Promote-in-place is based on usage, whereas evacuation is based on live.  The promote in place
// effort has to read the header of each marked object and has to write one header for each run of consecutive garbage
// objects.  Promote in place identifies runs of consecutive garbage by asking the marking context for the location of
// the next live object.
const uintx ShenandoahThrottler::PROMOTE_IN_PLACE_FACTOR = 16;

// The effort to update remembered set memory is assumed to be 8 times easier than updating memory without a remembered set.
// TODO: make this adaptive.
const uintx ShenandoahThrottler::REMEMBERED_SET_UPDATE_FACTOR = 8;

ShenandoahThrottler::ShenandoahThrottler(ShenandoahHeap* heap) :
    _heap(heap),
    _is_generational(heap->mode()->is_generational()),
    _evacuate_vs_update_factor(INITIAL_EVACUATE_VS_UPDATE_FACTOR),
#ifdef KELVIN_THROTTLES
    _threads_in_throttle(0),
#endif
    _most_recent_live_young_words(0),
    _most_recent_live_global_words(0),
    _progress(0) {
}

void ShenandoahThrottler::publish_metrics_and_increment_epoch(GCPhase id) {
  // Accumulations deal with multiple Idle phases.
  _heap->absorb_throttle_metrics_and_increment_epoch(_progress, id);
}

const char* ShenandoahThrottler::phase_name(GCPhase p) {
  switch (p) {
    case _idle:    return "Idle";
    case _reset:   return "Reset";
    case _mark:    return "Mark";
    case _evac:    return "Evac";
    case _update:  return "Update";
    default:       return "NoName";
  }
}

void ShenandoahThrottler::setup_for_mark(size_t words_allocatable, bool is_global) {
  assert(ShenandoahThrottleAllocations, "Only be here when allocation throttling is enabled");
  publish_metrics_and_increment_epoch(_mark);

  // During marking, work progress is represented by total words marked.  Accumulation of marked words during marking
  // is not linear.  During the initial stages of marking, almost every object seen has not yet been marked.  During
  // later stages of marking, the large majority of objects seen have already been marked, so accumulation of work
  // progress slows.  On the other hand, we are generally conservative in our estimate of how much total work needs to
  // be accomplished, so the marking effort will usually complete before the slower progress at the end of this phase
  // causes pacing to aggressively slow mutator allocations.
  //
  // Bottom line: our configuration of pacing during concurrent mark favors greedy behavior.  We strive not to stall
  // allocators unless we are in "dire straits".
  size_t projected_work;
  if (is_global) {
    size_t global_used = ShenandoahHeap::heap()->global_generation()->used() >> LogHeapWordSize;
    if (_most_recent_live_global_words == 0) {
      // No history yet.  Assume everything needs to be marked. During initialization, a high percentage of objects will
      // be persistent, so this is a safe conservative assumption.  Note that we do not have to "mark" objects that are
      // above TAMS.
      projected_work = global_used;
    } else {
      projected_work = MIN2(3 * _most_recent_live_global_words / 2, global_used);
    }
  } else {
    size_t young_used = ShenandoahHeap::heap()->young_generation()->used() >> LogHeapWordSize;
    if (_most_recent_live_young_words == 0) {
      // No history yet.  Assume everything needs to be marked. During initialization, a high percentage of objects will
      // be persistent, so this is a safe conservative assumption.  Note that we do not have to "mark" objects that are
      // above TAMS.
      projected_work = young_used;
    } else {
      // During certain workload spikes, the amount of marking effort has been observed to increase by over 6 fold
      // from one GC to the next.  Experimentation reveals it is generally not practical to recover from this scenario
      // if the triggering heuristic had not had the foresight to start the GC with a sufficiently long runway.  Instead,
      // our objective is to use throttling to handle up to 1.5 fold increase in marking effort, and to arrange that
      // throttling "fails fast" so we can quickly degenerate if more than 1.5 fold increase in marking is required.
      //
      // Outline of strategy::
      // 1. We allow 7/8 of memory available to be allocated during the planned mark effort
      // 2. We recognize that marking makes slower progress during its initial efforts than during its finishing efforts.
      //    Progress is measured by the number of words scanned within previously marked objects.  Scanning is much more
      //    work at the start of marking because, at the start, nearly every scanned reference points to an object that
      //    has not yet been marked, so we have to do the work of marking the object.  Near the end of marking, most
      //    references scanned refer to objects that have already been marked, so less effort is required of the scanner.
      //    For this reason, we bias our efforts to count smaller amounts of progress at the start as equivalent to
      //    larger amounts of progress at the end.  For this reason, we allow less measured progress at the start of
      //    marking than at the end.  This is staggered because typical marking efforts will finish after only 8 steps,
      //    some after 9, some after 10, and so on.
      // 3. If we complete all of the planned effort and we are still not done with the marking effort, we allot 32 times
      //    the remaining available memory.  This essentially turns off further throttling.  Eventually, a mutator
      //    experiences an allocation failure and we degenerate.  This would be the normal (without throttling) behavior
      //    in this scenario.  Presumably, because we have granted memory so liberally throughout the marking process,
      //    there has been no throttling to delay the start of the degenerated cycle.
      //
      // TODO: we can monitor for any given workload the maximum increase in marking effort between consecutive GCs
      // and adapt this number as appropriate.
      projected_work = MIN2(3 * _most_recent_live_young_words / 2, young_used);
    }
  }

  // What happens if there's more to mark than our projected work?  The allocation budget will run out and will
  // not be further replenished by completion of additional work increments.  That means more mutator threads will
  // find themselves in "infinite" waiting loops until we complete this phase of GC, at which time budgets will be
  // replenished.

  size_t phase_budget = 7 * words_allocatable / 8;
  size_t remnant = words_allocatable - phase_budget;

  assert(_Microphase_Count == _16th_microphase + 1, "Otherwise, the initializations that follow are not correct.");

  // Note that we typically overbudget work for marking by a factor of 6 (usually, all work is completed at 17-50% of projected)
  // We intentionally sum to 100%, because rare scenarios may require even more than projected_work to complete
  _work_completed[_first_microphase]    = (size_t) (0.031250 * projected_work);   // add 1/32
  _work_completed[_second_microphase]   = (size_t) (0.062500 * projected_work);   // add 1/32
  _work_completed[_third_microphase]    = (size_t) (0.093750 * projected_work);   // add 1/32
  _work_completed[_fourth_microphase]   = (size_t) (0.156250 * projected_work);   // add 1/16
  _work_completed[_fifth_microphase]    = (size_t) (0.218800 * projected_work);   // add 1/16
  _work_completed[_sixth_microphase]    = (size_t) (0.281250 * projected_work);   // add 1/16
  _work_completed[_seventh_microphase]  = (size_t) (0.343750 * projected_work);   // add 1/16
  _work_completed[_eighth_microphase]   = (size_t) (0.406250 * projected_work);   // add 1/16 
  _work_completed[_ninth_microphase]    = (size_t) (0.468750 * projected_work);   // add 1/16
  _work_completed[_tenth_microphase]    = (size_t) (0.531250 * projected_work);   // add 1/16
  _work_completed[_11th_microphase]     = (size_t) (0.609375 * projected_work);   // add 1/16 + 1/64
  _work_completed[_12th_microphase]     = (size_t) (0.687500 * projected_work);   // add 1/16 + 1/64
  _work_completed[_13th_microphase]     = (size_t) (0.765625 * projected_work);   // add 1/16 + 1/64
  _work_completed[_14th_microphase]     = (size_t) (0.843750 * projected_work);   // add 1/16 + 1/64
  _work_completed[_15th_microphase]     = (size_t) (0.921875 * projected_work);   // add 1/16 + 1/64
  _work_completed[_16th_microphase]     = (size_t) (1.000000 * projected_work);   // add 1/16 + 1/64


  // The most common case is that marking is completed by the end of twelfth microphase, so front-load the budget.
  // Initial allocation budget is 0.25 * phase_budget
  intptr_t initial_budget = phase_budget / 4;
  _budget_supplement[_first_microphase]    = (size_t) (0.093800 * phase_budget);  // + 3/32: Incremental = 0.0938 * phase_budget
  _budget_supplement[_second_microphase]   = (size_t) (0.093800 * phase_budget);  // + 3/32: Incremental = 0.1875 * phase_budget
  _budget_supplement[_third_microphase]    = (size_t) (0.093800 * phase_budget);  // + 3/32: Incremental = 0.2813 * phase_budget
  _budget_supplement[_fourth_microphase]   = (size_t) (0.093800 * phase_budget);  // + 3/32: Incremental = 0.3750 * phase_budget
  _budget_supplement[_fifth_microphase]    = (size_t) (0.062600 * phase_budget);  // + 2/32: Incremental = 0.4375 * phase_budget
  _budget_supplement[_sixth_microphase]    = (size_t) (0.046875 * phase_budget);  // + 3/64: Incremental = 0.4844 * phase_budget
  _budget_supplement[_seventh_microphase]  = (size_t) (0.046875 * phase_budget);  // + 3/64: Incremental = 0.5313 * phase_budget
  _budget_supplement[_eighth_microphase]   = (size_t) (0.046875 * phase_budget);  // + 3/64: Incremental = 0.5781 * phase_budget
  _budget_supplement[_ninth_microphase]    = (size_t) (0.031300 * phase_budget);  // + 1/32: Incremental = 0.6094 * phase_budget
  _budget_supplement[_tenth_microphase]    = (size_t) (0.031300 * phase_budget);  // + 1/32: Incremental = 0.6406 * phase_budget
  _budget_supplement[_11th_microphase]     = (size_t) (0.031300 * phase_budget);  // + 1/32: Incremental = 0.6719 * phase_budget
  _budget_supplement[_12th_microphase]     = (size_t) (0.031300 * phase_budget);  // + 1/32: Incremental = 0.7031 * phase_budget
  _budget_supplement[_13th_microphase]     = (size_t) (0.015625 * phase_budget);  // + 1/64: Incremental = 0.7188 * phase_budget
  _budget_supplement[_14th_microphase]     = (size_t) (0.015625 * phase_budget);  // + 1/64: Incremental = 0.7344 * phase_budget
  _budget_supplement[_15th_microphase]     = (size_t) (0.015625 * phase_budget);  // + 1/64: Incremental = 0.7500 * phase_budget
  _budget_supplement[_16th_microphase]     = 32 * remnant;                        // turn off throttling to enable degen

  Atomic::store(&_progress, (size_t) 0L);
  ShenandoahHeap::heap()->start_throttle_for_gc_phase(_mark, initial_budget, phase_budget, projected_work);
}

void ShenandoahThrottler::setup_for_evac(size_t allocatable_words, size_t evac_words, size_t promo_in_place_words,
                                         size_t uncollected_young_words, size_t uncollected_old_words,
                                         bool is_mixed, bool is_global, bool is_bootstrap) {
  assert(ShenandoahThrottleAllocations, "Only be here when pacing is enabled");
  publish_metrics_and_increment_epoch(_evac);

  // Note that promo_in_place words are initially part of uncollected_young_words, but they end up as uncollected_old_words
  //
  // The most meaningful estimate of the effort required to perform the next mark effort is the effort required
  // to perform this GC.  Note that uncollected_young_words also includes all the floating garbage that resides
  // above top_at_mark_start.  This does not have to be marked throught, and its accounting is not included in
  // progress.
  if (is_global) {
    _most_recent_live_global_words = _progress;
    if ((evac_words + uncollected_young_words + uncollected_old_words) > _most_recent_live_global_words) {
      size_t floating_garbage_estimate =
        (evac_words + uncollected_young_words + uncollected_old_words) - _most_recent_live_global_words;
      if (evac_words + uncollected_young_words > floating_garbage_estimate) {
        _most_recent_live_young_words = (evac_words + uncollected_young_words) - floating_garbage_estimate;
      } else {
        _most_recent_live_young_words = evac_words + uncollected_young_words;
      }
    } else {
      _most_recent_live_young_words = evac_words + uncollected_young_words;
    }
  } else if (!is_bootstrap) {
    _most_recent_live_young_words = _progress;
    if (evac_words + uncollected_young_words > _most_recent_live_young_words) {
      size_t floating_garbage_estimate = (evac_words + uncollected_young_words) - _most_recent_live_young_words;;
      if (evac_words + uncollected_young_words + uncollected_old_words > floating_garbage_estimate) {
        _most_recent_live_global_words =
          (evac_words + uncollected_young_words + uncollected_old_words) - floating_garbage_estimate;
      } else {
        _most_recent_live_global_words = (evac_words + uncollected_young_words + uncollected_old_words);
      }
    } else {
      _most_recent_live_global_words = (evac_words + uncollected_young_words + uncollected_old_words);
    }
  }
  // else is_bootstrap.  Bootstrap cycle has extra marking work, which is a combination of young and old.
  // Do not update _most_recent values.

  size_t evac_cost = evac_words + promo_in_place_words / PROMOTE_IN_PLACE_FACTOR;
  size_t update_cost;
  bool is_mixed_or_global = is_mixed || is_global;
  if (is_mixed_or_global) {
    update_cost = (size_t) ((uncollected_young_words + uncollected_old_words) / _evacuate_vs_update_factor);
  } else {
    update_cost = (size_t) ((((uncollected_old_words + promo_in_place_words) / REMEMBERED_SET_UPDATE_FACTOR) + 
                             (uncollected_young_words - promo_in_place_words)) / _evacuate_vs_update_factor);
  }
  
  double evac_fraction_of_total = ((double) evac_cost) / (evac_cost + update_cost);
  size_t projected_work = (size_t) evac_cost;
  size_t phase_budget = (size_t) (allocatable_words * evac_fraction_of_total);

  assert(_Microphase_Count == _16th_microphase + 1, "Otherwise, the initializations that follow are not correct.");

  _work_completed[_first_microphase]    = (size_t) (0.03125 * projected_work);   // 1/32 of planned work
  _work_completed[_second_microphase]   = (size_t) (0.09385 * projected_work);   // 1/16 of planned work
  _work_completed[_third_microphase]    = (size_t) (0.15625 * projected_work);   // 1/16 of planned work
  _work_completed[_fourth_microphase]   = (size_t) (0.21875 * projected_work);   // 1/16 of planned work
  _work_completed[_fifth_microphase]    = (size_t) (0.28125 * projected_work);   // 1/16 of planned work
  _work_completed[_sixth_microphase]    = (size_t) (0.34375 * projected_work);   // 1/16 of planned work
  _work_completed[_seventh_microphase]  = (size_t) (0.40625 * projected_work);   // 1/16 of planned work
  _work_completed[_eighth_microphase]   = (size_t) (0.46875 * projected_work);   // 1/16 of planned work
  _work_completed[_ninth_microphase]    = (size_t) (0.50000 * projected_work);   // 1/32 of planned work
  _work_completed[_tenth_microphase]    = (size_t) (0.56250 * projected_work);   // 1/16 of planned work
  _work_completed[_11th_microphase]     = (size_t) (0.62500 * projected_work);   // 1/16 of planned work
  _work_completed[_12th_microphase]     = (size_t) (0.68750 * projected_work);   // 1/16 of planned work
  _work_completed[_13th_microphase]     = (size_t) (0.75000 * projected_work);   // 1/16 of planned work
  _work_completed[_14th_microphase]     = (size_t) (0.81250 * projected_work);   // 1/16 of planned work
  _work_completed[_15th_microphase]     = (size_t) (0.87500 * projected_work);   // 1/16 of planned work
  _work_completed[_16th_microphase]     = (size_t) (0.93750 * projected_work);   // 1/16 of planned work


  // Initial allocation budget is 0.25 * phase_budget
  size_t initial_budget = phase_budget / 4;
  _budget_supplement[_first_microphase]    = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.0938 * phase_budget
  _budget_supplement[_second_microphase]   = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.1875 * phase_budget
  _budget_supplement[_third_microphase]    = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.2813 * phase_budget
  _budget_supplement[_fourth_microphase]   = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.3750 * phase_budget
  _budget_supplement[_fifth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.4063 * phase_budget
  _budget_supplement[_sixth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.4375 * phase_budget
  _budget_supplement[_seventh_microphase]  = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.4688 * phase_budget
  _budget_supplement[_eighth_microphase]   = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5000 * phase_budget
  _budget_supplement[_ninth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5313 * phase_budget
  _budget_supplement[_tenth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5625 * phase_budget
  _budget_supplement[_11th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5938 * phase_budget
  _budget_supplement[_12th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.6250 * phase_budget
  _budget_supplement[_13th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.6563 * phase_budget
  _budget_supplement[_14th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.6875 * phase_budget
  _budget_supplement[_15th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.7188 * phase_budget
  _budget_supplement[_16th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.7500 * phase_budget

  Atomic::store(&_progress, (size_t) 0L);
  ShenandoahHeap::heap()->start_throttle_for_gc_phase(_evac, initial_budget, phase_budget, projected_work);
}

void ShenandoahThrottler::setup_for_updaterefs(size_t allocatable_words, size_t promo_in_place_words,
                                               size_t uncollected_young_words, size_t uncollected_old_words,
                                               bool is_mixed_or_global) {
  assert(ShenandoahThrottleAllocations, "Only be here when throttling is enabled");
  publish_metrics_and_increment_epoch(_update);

  // We use uncollected words to estimate the update-refs effort.  We are consistent between budgeting (here) and
  // reporting, as update-refs work is completed.  A more accurate estimate would be uncollected-words below
  // update-water-mark, but that value is not conveniently available without iterating over all regions.
  //
  // An unintended side effect of the current approach is that uncollected words continues to grow as concurrent
  // mutators continue to allocate.  This means the reported work is larger than the budgeted work.  This means
  // the budget supplements may be granted sooner than was originally intended.  In general, this is harmless.
  // The "only" potential negative impact is that throttle allocations earlier in the update-refs phase a bit
  // less and throttle allocations later in the update-refs phase a bit more.
  size_t update_cost;
  if (is_mixed_or_global) {
    update_cost = (size_t) ((uncollected_young_words + uncollected_old_words) / _evacuate_vs_update_factor);
  } else {
    update_cost = (size_t) ((((uncollected_old_words + promo_in_place_words) / REMEMBERED_SET_UPDATE_FACTOR) + 
                             (uncollected_young_words - promo_in_place_words)) / _evacuate_vs_update_factor);
  }
  
  size_t projected_work = update_cost;
  size_t phase_budget = allocatable_words;

  assert(_Microphase_Count == _16th_microphase + 1, "Otherwise, the initializations that follow are not correct.");

  _work_completed[_first_microphase]    = (size_t) (0.03125 * projected_work);   // 1/32 of planned work
  _work_completed[_second_microphase]   = (size_t) (0.09385 * projected_work);   // 1/16 of planned work
  _work_completed[_third_microphase]    = (size_t) (0.15625 * projected_work);   // 1/16 of planned work
  _work_completed[_fourth_microphase]   = (size_t) (0.21875 * projected_work);   // 1/16 of planned work
  _work_completed[_fifth_microphase]    = (size_t) (0.28125 * projected_work);   // 1/16 of planned work
  _work_completed[_sixth_microphase]    = (size_t) (0.34375 * projected_work);   // 1/16 of planned work
  _work_completed[_seventh_microphase]  = (size_t) (0.40625 * projected_work);   // 1/16 of planned work
  _work_completed[_eighth_microphase]   = (size_t) (0.46875 * projected_work);   // 1/16 of planned work
  _work_completed[_ninth_microphase]    = (size_t) (0.50000 * projected_work);   // 1/32 of planned work
  _work_completed[_tenth_microphase]    = (size_t) (0.56250 * projected_work);   // 1/16 of planned work
  _work_completed[_11th_microphase]     = (size_t) (0.62500 * projected_work);   // 1/16 of planned work
  _work_completed[_12th_microphase]     = (size_t) (0.68750 * projected_work);   // 1/16 of planned work
  _work_completed[_13th_microphase]     = (size_t) (0.75000 * projected_work);   // 1/16 of planned work
  _work_completed[_14th_microphase]     = (size_t) (0.81250 * projected_work);   // 1/16 of planned work
  _work_completed[_15th_microphase]     = (size_t) (0.87500 * projected_work);   // 1/16 of planned work
  _work_completed[_16th_microphase]     = (size_t) (0.93750 * projected_work);   // 1/16 of planned work

  // Initial allocation budget is 0.25 * phase_budget
  size_t initial_budget = phase_budget / 4;
  _budget_supplement[_first_microphase]    = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.0938 * phase_budget
  _budget_supplement[_second_microphase]   = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.1875 * phase_budget
  _budget_supplement[_third_microphase]    = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.2813 * phase_budget
  _budget_supplement[_fourth_microphase]   = (size_t) (0.0938 * phase_budget);  // + 3/32: Incremental = 0.3750 * phase_budget
  _budget_supplement[_fifth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.4063 * phase_budget
  _budget_supplement[_sixth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.4375 * phase_budget
  _budget_supplement[_seventh_microphase]  = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.4688 * phase_budget
  _budget_supplement[_eighth_microphase]   = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5000 * phase_budget
  _budget_supplement[_ninth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5313 * phase_budget
  _budget_supplement[_tenth_microphase]    = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5625 * phase_budget
  _budget_supplement[_11th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.5938 * phase_budget
  _budget_supplement[_12th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.6250 * phase_budget
  _budget_supplement[_13th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.6563 * phase_budget
  _budget_supplement[_14th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.6875 * phase_budget
  _budget_supplement[_15th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.7188 * phase_budget
  _budget_supplement[_16th_microphase]     = (size_t) (0.0313 * phase_budget);  // + 1/32: Incremental = 0.7500 * phase_budget

  Atomic::store(&_progress, (size_t) 0L);
  ShenandoahHeap::heap()->start_throttle_for_gc_phase(_update, initial_budget, phase_budget, projected_work);
}

/* This allocatable should be the headroom (available minus (allocation spike plus penalties).  In theory, the
 * triggering heuristic will start the next GC before we have exhausted what remains in allocatable.
 *
 * TODO: we could force a certain amount of old-gen marking to be completed before we allow the entirety of
 * allocatable to be exhausted.
 */
void ShenandoahThrottler::setup_for_idle(size_t allocatable_words) {
  assert(ShenandoahThrottleAllocations, "Only be here when throttling is enabled");
  publish_metrics_and_increment_epoch(_idle);
  
  assert(_Microphase_Count == _16th_microphase + 1, "Otherwise, the initializations that follow are not correct.");

  // No work will be recorded, so it doesn't really matter what value we store here.
  _work_completed[_first_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_second_microphase]  = (size_t) 0x7fffffffL;
  _work_completed[_third_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_fourth_microphase]  = (size_t) 0x7fffffffL;
  _work_completed[_fifth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_sixth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_seventh_microphase] = (size_t) 0x7fffffffL;
  _work_completed[_eighth_microphase]  = (size_t) 0x7fffffffL;
  _work_completed[_ninth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_tenth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_11th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_12th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_13th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_14th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_15th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_16th_microphase]    = (size_t) 0x7fffffffL;

  // Everything is budgeted now.  Nothing is held back;
  _budget_supplement[_first_microphase]   = (size_t) 0;
  _budget_supplement[_second_microphase]  = (size_t) 0;
  _budget_supplement[_third_microphase]   = (size_t) 0;
  _budget_supplement[_fourth_microphase]  = (size_t) 0;
  _budget_supplement[_fifth_microphase]   = (size_t) 0;
  _budget_supplement[_sixth_microphase]   = (size_t) 0;
  _budget_supplement[_seventh_microphase] = (size_t) 0;
  _budget_supplement[_eighth_microphase]  = (size_t) 0;
  _budget_supplement[_ninth_microphase]   = (size_t) 0;
  _budget_supplement[_tenth_microphase]   = (size_t) 0;
  _budget_supplement[_11th_microphase]    = (size_t) 0;
  _budget_supplement[_12th_microphase]    = (size_t) 0;
  _budget_supplement[_13th_microphase]    = (size_t) 0;
  _budget_supplement[_14th_microphase]    = (size_t) 0;
  _budget_supplement[_15th_microphase]    = (size_t) 0;
  _budget_supplement[_16th_microphase]    = (size_t) 0;

  // We don't want throttles during Idle, so we double available
  Atomic::store(&_progress, (size_t) 0L);
  ShenandoahHeap::heap()->start_throttle_for_gc_phase(_idle, allocatable_words * 2, allocatable_words * 2, 0);
}

/*
 * There is no useful notion of progress for this operation. To avoid stalling
 * the allocators unnecessarily, allow them to run unimpeded.
 */
void ShenandoahThrottler::setup_for_reset(size_t allocatable_words) {
  assert(ShenandoahThrottleAllocations, "Only be here when throttling is enabled");
  publish_metrics_and_increment_epoch(_reset);

  assert(_Microphase_Count == _16th_microphase + 1, "Otherwise, the initializations that follow are not correct.");

  // No work will be recorded, so it doesn't really matter what value we store here.
  _work_completed[_first_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_second_microphase]  = (size_t) 0x7fffffffL;
  _work_completed[_third_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_fourth_microphase]  = (size_t) 0x7fffffffL;
  _work_completed[_fifth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_sixth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_seventh_microphase] = (size_t) 0x7fffffffL;
  _work_completed[_eighth_microphase]  = (size_t) 0x7fffffffL;
  _work_completed[_ninth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_tenth_microphase]   = (size_t) 0x7fffffffL;
  _work_completed[_11th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_12th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_13th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_14th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_15th_microphase]    = (size_t) 0x7fffffffL;
  _work_completed[_16th_microphase]    = (size_t) 0x7fffffffL;

  // Everything is budgeted now.  Nothing is held back;
  _budget_supplement[_first_microphase]   = (size_t) 0;
  _budget_supplement[_second_microphase]  = (size_t) 0;
  _budget_supplement[_third_microphase]   = (size_t) 0;
  _budget_supplement[_fourth_microphase]  = (size_t) 0;
  _budget_supplement[_fifth_microphase]   = (size_t) 0;
  _budget_supplement[_sixth_microphase]   = (size_t) 0;
  _budget_supplement[_seventh_microphase] = (size_t) 0;
  _budget_supplement[_eighth_microphase]  = (size_t) 0;
  _budget_supplement[_ninth_microphase]   = (size_t) 0;
  _budget_supplement[_tenth_microphase]   = (size_t) 0;
  _budget_supplement[_11th_microphase]    = (size_t) 0;
  _budget_supplement[_12th_microphase]    = (size_t) 0;
  _budget_supplement[_13th_microphase]    = (size_t) 0;
  _budget_supplement[_14th_microphase]    = (size_t) 0;
  _budget_supplement[_15th_microphase]    = (size_t) 0;
  _budget_supplement[_16th_microphase]    = (size_t) 0;

  Atomic::store(&_progress, (size_t) 0L);
  ShenandoahHeap::heap()->start_throttle_for_gc_phase(_reset, allocatable_words, allocatable_words, 0);
}

// The typical root cause for degeneration is excessive growth of live memory without awareness by the triggering
// heuristic that triggering must be accelerated.  Now that we have experienced a degenerated cycle, the triggering
// mechanism will be more informed.  For the next cycle, let's assume live memory has tripled in size.
void ShenandoahThrottler::recover_from_degeneration(bool is_global) {
  if (is_global) {
    _most_recent_live_global_words *= 3;
  } else {
    _most_recent_live_young_words *= 3;
  }
}
