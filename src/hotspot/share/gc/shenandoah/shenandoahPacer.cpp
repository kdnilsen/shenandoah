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

// A value of 16 signifies that updating a region is 16 times easier than evacuating a region.  The value is
// determined empirically.  If we are more likely to throttle during evac than update refs, increase this value.
// If more likely to throttle during update refs than evacuate, decrease this number.  Note that evacauating has
// to read every value and write every value.  Updating only has to read reference values (N % of total live memory)
// and then it overwrites the same value in x% of cases (i.e. if the original value points to CSET).  Different workloads
// will behave differently.
//
// TODO: adjust this dynamically according to measurements of where we experience throttling and/or according to
// which phase has more memory available at the end of phase.
const uintx ShenandoahThrottler::EVACUATE_VS_UPDATE_FACTOR = 16;

// A value of 16 signifies that it is 16 times easier to process a promote-in-place region than to evacuate the live
// data within a region.  Promote-in-place is based on usage, whereas evacuation is based on live.  The promote in place
// effort has to read the header of each marked object and has to write one header for each run of consecutive garbage
// objects.  Promote in place identifies runs of consecutive garbage by asking the marking context for the location of
// the next live object.
const uintx ShenandoahThrottler::PROMOTE_IN_PLACE_FACTOR = 16;

// A value of 32 denotes that it is 32 times easier to update an old-gen region using the remembered set of dirty
// cards than it is to evacuate a region.
const uintx ShenandoahThrottler::REMEMBERED_SET_UPDATE_FACTOR = 32;


const uintx ShenandoahThrottler::MAX_THROTTLE_DELAY_MS = 128;
const uintx ShenandoahThrottler::MIN_THROTTLE_DELAY_MS = 2;

void ShenandoahThrottler::add_to_metrics(bool successful, size_t words, double delay) {

  size_t old_val, new_val;
  double old_dv, new_dv;
  do {
    old_val = Atomic::load(&_allocation_requests_throttled);
    new_val = old_val + 1;
  } while (Atomic::cmpxchg(&_allocation_requests_throttled, old_val, new_val, memory_order_relaxed) != old_val);
  if (successful) {

    // _total_words_throttled
    do {
      old_val = Atomic::load(&_total_words_throttled);
      new_val = old_val + words;
    } while (Atomic::cmpxchg(&_total_words_throttled, old_val, new_val, memory_order_relaxed) != old_val);

    // _max_time_throttled_per_successful_allocation
    do {
      old_dv = Atomic::load(&_max_time_throttled_per_successful_allocation);
      if (old_dv > delay) {
        break;
      }
      new_dv = delay;
    } while (Atomic::cmpxchg(&_max_time_throttled_per_successful_allocation, old_dv, new_dv, memory_order_relaxed) != old_dv);
  } else {

    // _total_words_failed_to_throttle
    do {
      old_val = Atomic::load(&_total_words_failed_to_throttle);
      new_val = old_val + words;
    } while (Atomic::cmpxchg(&_total_words_failed_to_throttle, old_val, new_val, memory_order_relaxed) != old_val);

    // _max_time_throttled_per_failed_allocation
    do {
      old_dv = Atomic::load(&_max_time_throttled_per_failed_allocation);
      if (old_dv > delay) {
        break;
      }
      new_dv = delay;
    } while (Atomic::cmpxchg(&_max_time_throttled_per_failed_allocation, old_dv, new_dv, memory_order_relaxed) != old_dv);
  }

  // _max_words_throttled_per_allocation
  do {
    old_val = Atomic::load(&_max_words_throttled_per_allocation);
    if (old_val > words) {
      break;
    }
    new_val = words;
  } while (Atomic::cmpxchg(&_max_words_throttled_per_allocation, old_val, new_val, memory_order_relaxed) != old_val);

  // _min_words_throttled_per_allocation
  do {
    old_val = Atomic::load(&_min_words_throttled_per_allocation);
    if (old_val < words) {
      break;
    }
    new_val = words;
  } while (Atomic::cmpxchg(&_min_words_throttled_per_allocation, old_val, new_val, memory_order_relaxed) != old_val);

  // _total_time_in_throttle
  do {
    old_dv = Atomic::load(&_total_time_in_throttle);
    new_dv = old_dv + delay;;
  } while (Atomic::cmpxchg(&_total_time_in_throttle, old_dv, new_dv, memory_order_relaxed) != old_dv);
}

void ShenandoahThrottler::reset_metrics(const char *label) {
  _phase_label = label;
  _allocation_requests_throttled = 0;
  _total_words_throttled = 0;
  _total_words_failed_to_throttle = 0;
  _max_words_throttled_per_allocation = 0;
  _min_words_throttled_per_allocation = SIZE_MAX;
  _total_time_in_throttle = 0.0;
  _max_time_throttled_per_successful_allocation = 0.0;
  _max_time_throttled_per_failed_allocation = 0.0;
}

void ShenandoahThrottler::publish_metrics() {
  if (_phase_label != nullptr) {
    size_t bytes_available = _available_words << LogHeapWordSize;
    size_t progress_in_bytes = _progress << LogHeapWordSize;
    log_info(gc)("Throttle report at end of %s (available: " SIZE_FORMAT "%s after progress: " SIZE_FORMAT "%s):",  _phase_label,
                 byte_size_in_proper_unit(bytes_available),    proper_unit_for_byte_size(bytes_available),
                 byte_size_in_proper_unit(progress_in_bytes),  proper_unit_for_byte_size(progress_in_bytes));
    size_t bytes_throttled = _total_words_throttled << LogHeapWordSize;
// Probably not worth the effort to monitor these...
//    log_info(gc)("    threads in throttle: %8ld", Atomic::load(&_threads_in_throttle));
//
    log_info(gc)("  throttled allocations: %8ld, total bytes throttled: %8ld%s",
                 _allocation_requests_throttled,
                 byte_size_in_proper_unit(bytes_throttled), proper_unit_for_byte_size(bytes_throttled));
    size_t min_bytes_throttled =
      (_min_words_throttled_per_allocation == SIZE_MAX)? 0: _min_words_throttled_per_allocation << LogHeapWordSize;
    size_t max_bytes_throttled = _max_words_throttled_per_allocation << LogHeapWordSize;
    log_info(gc)("   min throttled bytes: %8ld%s,   max throttled bytes: %8ld%s",
                 byte_size_in_proper_unit(min_bytes_throttled), proper_unit_for_byte_size(min_bytes_throttled),
                 byte_size_in_proper_unit(max_bytes_throttled), proper_unit_for_byte_size(max_bytes_throttled));
    size_t bytes_failed_to_throttle = _total_words_failed_to_throttle << LogHeapWordSize;
    log_info(gc)("  Total bytes that failed to throttle: %8ld%s",
                 byte_size_in_proper_unit(bytes_failed_to_throttle), proper_unit_for_byte_size(bytes_failed_to_throttle));
    log_info(gc)("    max time in successful allocation: %12.6fs, in failed allocation: %12.6fs",
                 _max_time_throttled_per_successful_allocation, _max_time_throttled_per_failed_allocation);
    log_info(gc)("                  total throttle time: %12.6fs", _total_time_in_throttle);
  }
  // else, this is the start of very first phase, and there is no prior phase to be reported
}

void ShenandoahThrottler::setup_for_mark(size_t words_allocatable, bool is_global) {
  assert(ShenandoahThrottleAllocations, "Only be here when allocation throttling is enabled");
  publish_metrics();
  Atomic::inc(&_epoch);

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
    size_t global_used = ShenandoahHeap::heap()->global_generation()->used();
    if (_most_recent_live_global_words == 0) {
      // No history yet.  Assume everything needs to be marked. During initialization, a high percentage of objects will
      // be persistent, so this is a safe conservative assumption.  Note that we do not have to "mark" objects that are
      // above TAMS.
      projected_work = global_used;
    } else {
      projected_work = MIN2(_most_recent_live_global_words * 2, global_used);
    }
  } else {
    size_t young_used = ShenandoahHeap::heap()->young_generation()->used();
    if (_most_recent_live_young_words == 0) {
      // No history yet.  Assume everything needs to be marked. During initialization, a high percentage of objects will
      // be persistent, so this is a safe conservative assumption.  Note that we do not have to "mark" objects that are
      // above TAMS.
      projected_work = young_used;
    } else {
      projected_work = MIN2(_most_recent_live_young_words * 2, young_used);
    }
  }

  // What happens if there's more to mark than our projected work?  The allocation budget will run out and will
  // not be further replenished by completion of additional work increments.  That means more mutator threads will
  // find themselves in "infinite" waiting loops until we complete this phase of GC, at which time budgets will be
  // replenished.


  // Allow ourselves to allocate 3/4 of remaining available during concurrent mark, recognizing that concurrent
  // marking is typically the most time consuming phase of GC (e.g. 70%).  Recognize that any immediate trash
  // reclaimed at the end of concurrent marking will allow us to expand the budget when we begin concurrent evacuation.
  // In other words, allowing 75% of available to be allocated during concurrent marking is probably conservative.

  size_t phase_budget = 3 * words_allocatable / 4;

  assert(ShenandoahThrottleBudgetSegmentsPerPhase == 3, "Otherwise, the initializations that follow are not correct.");

  _work_completed[0] = (size_t) (0.125 * projected_work); // first 1/8 of projected work
  _work_completed[1] = (size_t) (0.25 * projected_work);  // 1/8 more
  _work_completed[2] = (size_t) (0.5 * projected_work);   // 1/4 more

  // Initial allocation budget is 0.5 * phase_budget
  intptr_t initial_budget = phase_budget / 2;
  _budget_supplement[0] = (size_t) (0.25 * phase_budget);   // Cumulative budget = 0.75 * phase_budget
  _budget_supplement[1] = (size_t) (0.125 * phase_budget);  // Cumulative budget = 0.875 * phase_budget
  _budget_supplement[2] = (size_t) (0.125 * phase_budget);  // Cumulative budget = 1.0 * phase_budget

#ifdef KELVIN_DEPRECATE
  Atomic::store(&_authorized_allocations, initial_budget);
  Atomic::store(&_allocated, (size_t) 0L);
#endif
  Atomic::store(&_available_words, initial_budget);
  Atomic::store(&_progress, (intptr_t) 0L);
  reset_metrics("Mark");
  wake_throttled();

  size_t expected_live = projected_work << LogHeapWordSize;
  size_t phase_budget_bytes = phase_budget << LogHeapWordSize;
  size_t bytes_authorized = initial_budget << LogHeapWordSize;
  log_info(gc, ergo)("Throttle Marking Phase Budget: " SIZE_FORMAT "%s, immediate authorization: " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(phase_budget_bytes),  proper_unit_for_byte_size(phase_budget_bytes),
                     byte_size_in_proper_unit(bytes_authorized),    proper_unit_for_byte_size(bytes_authorized));
  log_info(gc, ergo)(" Planning to mark " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(expected_live),       proper_unit_for_byte_size(expected_live));

  for (int i = 0; i < ShenandoahThrottleBudgetSegmentsPerPhase; i++) {
    size_t byte_budget = _budget_supplement[i] << LogHeapWordSize;
    size_t bytes_worked = _work_completed[i] << LogHeapWordSize;
    log_info(gc, ergo)(" Supplement[%d]: " SIZE_FORMAT "%s, planned when total work completed is: " SIZE_FORMAT "%s", i,
                       byte_size_in_proper_unit(byte_budget),       proper_unit_for_byte_size(byte_budget),
                       byte_size_in_proper_unit(bytes_worked),      proper_unit_for_byte_size(bytes_worked));
  }
}

void ShenandoahThrottler::setup_for_evac(size_t allocatable_words, size_t evac_words, size_t promo_in_place_words,
                                         size_t uncollected_young_words, size_t uncollected_old_words,
                                         bool is_mixed, bool is_global) {
  assert(ShenandoahThrottleAllocations, "Only be here when pacing is enabled");
  publish_metrics();
  Atomic::inc(&_epoch);

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
  } else {
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

  size_t evac_cost = evac_words + promo_in_place_words / PROMOTE_IN_PLACE_FACTOR;
  size_t update_cost;
  bool is_mixed_or_global = is_mixed || is_global;
  if (is_mixed_or_global) {
    update_cost = (uncollected_young_words + uncollected_old_words) / EVACUATE_VS_UPDATE_FACTOR;
  } else {
    update_cost = ((uncollected_young_words - promo_in_place_words) / EVACUATE_VS_UPDATE_FACTOR +
                   (uncollected_old_words + promo_in_place_words) / REMEMBERED_SET_UPDATE_FACTOR);
  }
  
  double evac_fraction_of_total = ((double) evac_cost) / (evac_cost + update_cost);
  size_t projected_work = (size_t) evac_cost;
  size_t phase_budget = (size_t) (allocatable_words * evac_fraction_of_total);

  assert(ShenandoahThrottleBudgetSegmentsPerPhase == 3, "Otherwise, the initializations that follow are not correct.");

  _work_completed[0] = (size_t) (0.25 * projected_work);  // first 1/4 of projected work
  _work_completed[1] = (size_t) (0.50 * projected_work);  // 1/4 more
  _work_completed[2] = (size_t) (0.75 * projected_work);  // 1/4 more

  // Initial allocation budget is 0.5 * phase_budget
  size_t initial_budget = phase_budget / 2;
  _budget_supplement[0] = (size_t) (0.25 * phase_budget);   // Cumulative budget = 0.75 * phase_budget
  _budget_supplement[1] = (size_t) (0.125 * phase_budget);  // Cumulative budget = 0.875 * phase_budget
  _budget_supplement[2] = (size_t) (0.125 * phase_budget);  // Cumulative budget = 1.0 * phase_budget

#ifdef KELVIN_DEPRECATE
  Atomic::store(&_authorized_allocations, initial_budget);
  Atomic::store(&_allocated, (size_t) 0);
#endif
  Atomic::store(&_available_words, (intptr_t) initial_budget);
  Atomic::store(&_progress, (intptr_t) 0);
  reset_metrics("Evacuation");
  wake_throttled();

  size_t bytes_to_evac = projected_work << LogHeapWordSize;
  size_t phase_budget_bytes = phase_budget << LogHeapWordSize;
  size_t bytes_authorized = initial_budget << LogHeapWordSize;
  log_info(gc, ergo)("Throttle Evacuation (%s) Phase Budget: " SIZE_FORMAT "%s, immediate authorization: " SIZE_FORMAT "%s",
                     is_mixed_or_global? "mixed or global evacuation ": "young",
                     byte_size_in_proper_unit(phase_budget_bytes),  proper_unit_for_byte_size(phase_budget_bytes),
                     byte_size_in_proper_unit(bytes_authorized),    proper_unit_for_byte_size(bytes_authorized));
  log_info(gc, ergo)(" Evacuating " SIZE_FORMAT "%s bytes",
                     byte_size_in_proper_unit(bytes_to_evac),       proper_unit_for_byte_size(bytes_to_evac));
  for (int i = 0; i < ShenandoahThrottleBudgetSegmentsPerPhase; i++) {
    size_t byte_budget = _budget_supplement[i] << LogHeapWordSize;
    size_t bytes_worked = _work_completed[i] << LogHeapWordSize;
    log_info(gc, ergo)(" Supplement[%d]: " SIZE_FORMAT "%s, planned when total work completed is: " SIZE_FORMAT "%s", i,
                       byte_size_in_proper_unit(byte_budget),       proper_unit_for_byte_size(byte_budget),
                       byte_size_in_proper_unit(bytes_worked),      proper_unit_for_byte_size(bytes_worked));
  }
}

void ShenandoahThrottler::setup_for_updaterefs(size_t allocatable_words, size_t promo_in_place_words,
                                               size_t uncollected_young_words, size_t uncollected_old_words,
                                               bool is_mixed_or_global) {
  assert(ShenandoahThrottleAllocations, "Only be here when throttling is enabled");
  publish_metrics();
  Atomic::inc(&_epoch);

  size_t update_cost;
  if (is_mixed_or_global) {
    update_cost = (uncollected_young_words + uncollected_old_words) / EVACUATE_VS_UPDATE_FACTOR;
  } else {
    update_cost = ((uncollected_young_words - promo_in_place_words) / EVACUATE_VS_UPDATE_FACTOR +
                   (uncollected_old_words + promo_in_place_words) / REMEMBERED_SET_UPDATE_FACTOR);
  }
  
  size_t projected_work = update_cost;
  size_t phase_budget = allocatable_words;

  assert(ShenandoahThrottleBudgetSegmentsPerPhase == 3, "Otherwise, the initializations that follow are not correct.");

  _work_completed[0] = (size_t) (0.25 * projected_work);  // first 1/4 of projected work
  _work_completed[1] = (size_t) (0.50 * projected_work);  // 1/4 more
  _work_completed[2] = (size_t) (0.75 * projected_work);  // 1/4 more

  // Initial allocation budget is 0.5 * phase_budget
  size_t initial_budget = phase_budget / 2;
  _budget_supplement[0] = (size_t) (0.25 * phase_budget);   // Cumulative budget = 0.75 * phase_budget
  _budget_supplement[1] = (size_t) (0.125 * phase_budget);  // Cumulative budget = 0.875 * phase_budget
  _budget_supplement[2] = (size_t) (0.125 * phase_budget);  // Cumulative budget = 1.0 * phase_budget

#ifdef KELVIN_DEPRECATE
  Atomic::store(&_authorized_allocations, initial_budget);
  Atomic::store(&_allocated, (size_t) 0);
#endif
  Atomic::store(&_available_words, (intptr_t) initial_budget);
  Atomic::store(&_progress, (intptr_t) 0);
  reset_metrics("Update References");
  wake_throttled();

  size_t young_bytes = (uncollected_young_words - promo_in_place_words) << LogHeapWordSize;
  size_t old_bytes = (uncollected_old_words + promo_in_place_words) << LogHeapWordSize;
  size_t phase_budget_bytes = phase_budget << LogHeapWordSize;
  size_t bytes_authorized = initial_budget << LogHeapWordSize;
  log_info(gc, ergo)("Throttle (%s cycle) Updating Refs Phase Budget: "
                     SIZE_FORMAT "%s, immediate authorization: " SIZE_FORMAT "%s",
                     is_mixed_or_global? "mixed or global evacuation ": "young",
                     byte_size_in_proper_unit(phase_budget_bytes),  proper_unit_for_byte_size(phase_budget_bytes),
                     byte_size_in_proper_unit(bytes_authorized),    proper_unit_for_byte_size(bytes_authorized));
  log_info(gc, ergo)(" Uncollected Young: " SIZE_FORMAT "%s, Uncollected Old: " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(young_bytes),         proper_unit_for_byte_size(young_bytes),
                     byte_size_in_proper_unit(old_bytes),           proper_unit_for_byte_size(old_bytes));

  for (int i = 0; i < ShenandoahThrottleBudgetSegmentsPerPhase; i++) {
    size_t byte_budget = _budget_supplement[i] << LogHeapWordSize;
    size_t bytes_worked = _work_completed[i] << LogHeapWordSize;
    log_info(gc, ergo)(" Supplement[%d]: " SIZE_FORMAT "%s, planned when total work completed is: " SIZE_FORMAT "%s", i,
                       byte_size_in_proper_unit(byte_budget),       proper_unit_for_byte_size(byte_budget),
                       byte_size_in_proper_unit(bytes_worked),      proper_unit_for_byte_size(bytes_worked));
  }
}

/* This allocatable should be the headroom (available minus (allocation spike plus penalties).  In theory, the
 * triggering heuristic will start the next GC before we have exhausted what remains in allocatable.
 *
 * TODO: we could force a certain amount of old-gen marking to be completed before we allow the entirety of
 * allocatable to be exhausted.
 */
void ShenandoahThrottler::setup_for_idle(size_t allocatable_words) {
  assert(ShenandoahThrottleAllocations, "Only be here when throttling is enabled");
  publish_metrics();
  Atomic::inc(&_epoch);
  
  assert(ShenandoahThrottleBudgetSegmentsPerPhase == 3, "Otherwise, the initializations that follow are not correct.");

  // No work will be recorded, so it doesn't really matter what value we store here.
  _work_completed[0] = (size_t) 0x7fffffffL;
  _work_completed[1] = (size_t) 0x7fffffffL;
  _work_completed[2] = (size_t) 0x7fffffffL;

  // Everything is budgeted now.  Nothing is held back;
  _budget_supplement[0] = (size_t) 0;
  _budget_supplement[1] = (size_t) 0;
  _budget_supplement[2] = (size_t) 0;
  reset_metrics("Idle");
  wake_throttled();

#ifdef KELVIN_DEPRECATE
  Atomic::store(&_authorized_allocations, allocatable_words);
  Atomic::store(&_allocated, (size_t) 0);
#endif
  Atomic::store(&_available_words, (intptr_t) allocatable_words);
  Atomic::store(&_progress, (intptr_t) 0);

  size_t bytes_authorized = allocatable_words << LogHeapWordSize;
  log_info(gc, ergo)("Throttle Idle Phase Budget: " SIZE_FORMAT "%s (all authorized now)",
                     byte_size_in_proper_unit(bytes_authorized),      proper_unit_for_byte_size(bytes_authorized));
  /*
   * No value in dumping supplementals.
   */
}

/*
 * There is no useful notion of progress for this operation. To avoid stalling
 * the allocators unnecessarily, allow them to run unimpeded.
 */
void ShenandoahThrottler::setup_for_reset(size_t allocatable_words) {
  assert(ShenandoahThrottleAllocations, "Only be here when throttling is enabled");
  publish_metrics();
  Atomic::inc(&_epoch);

  assert(ShenandoahThrottleBudgetSegmentsPerPhase == 3, "Otherwise, the initializations that follow are not correct.");

  // No work will be recorded, so it doesn't really matter what value we store here.
  _work_completed[0] = (size_t) 0x7fffffffL;
  _work_completed[1] = (size_t) 0x7fffffffL;
  _work_completed[2] = (size_t) 0x7fffffffL;

  // Everything is budgeted now.  Nothing is held back;
  _budget_supplement[0] = (size_t) 0;
  _budget_supplement[1] = (size_t) 0;
  _budget_supplement[2] = (size_t) 0;

#ifdef KELVIN_DEPRECATE
  Atomic::store(&_authorized_allocations, allocatable_words);
  Atomic::store(&_allocated, (size_t) 0);
#endif
  Atomic::store(&_available_words, (intptr_t) allocatable_words);
  Atomic::store(&_progress, (intptr_t) 0);
  reset_metrics("Reset");
  wake_throttled();

  size_t bytes_authorized = allocatable_words << LogHeapWordSize;
  log_info(gc, ergo)("Throttle Reset Phase Budget: " SIZE_FORMAT "%s (all authorized now)",
                     byte_size_in_proper_unit(bytes_authorized),    proper_unit_for_byte_size(bytes_authorized));
  /*
   * No value in dumping supplementals.
   */
}

#ifdef KELVIN_DOES_NOT_NEED
size_t ShenandoahThrottler::update_and_get_progress_history() {
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

void ShenandoahThrottler::restart_with(size_t non_taxable_bytes, double tax_rate) {
  size_t initial = (size_t)(non_taxable_bytes * tax_rate) >> LogHeapWordSize;
  STATIC_ASSERT(sizeof(size_t) <= sizeof(intptr_t));
  Atomic::xchg(&_budget, (intptr_t)initial, memory_order_relaxed);
  Atomic::store(&_tax_rate, tax_rate);
  Atomic::inc(&_epoch);

  // Shake up stalled waiters after budget update.
  _need_notify_waiters.try_set();
}
#endif

bool ShenandoahThrottler::claim_for_alloc(intptr_t words, bool force, bool allow_greed) {
  assert(ShenandoahThrottleAllocations, "Only be here when throttling is enabled");

  intptr_t orig_budget, new_budget;
  do {
    orig_budget = Atomic::load(&_available_words);
    if (!allow_greed && (words * 2 > orig_budget)) {
      // Don't let a very large allocation consume more than half the remaining budget until we have allowed smaller requests
      return false;
    } else if (force || (orig_budget - words >= 0)) {
      new_budget = orig_budget - words;
    } else {
      // GC has not yet earned the right to allocate this memory
      return false;
    }
  } while (Atomic::cmpxchg(&_available_words, orig_budget, new_budget, memory_order_relaxed) != orig_budget);
  return true;
}

#undef KELVIN_THROTTLE

// We unthrottle when an authorized allocation consumes less than was authorized.
void ShenandoahThrottler::unthrottle_for_alloc(intptr_t epoch, size_t words) {
  assert(ShenandoahThrottleAllocations, "Only be here when pacing is enabled");

  if (Atomic::load(&_epoch) != epoch) {
    // Stale ticket, no need to unpace.
    return;
  }

#ifdef KELVIN_THROTTLE
  JavaThread* current_thread = JavaThread::current();
  log_info(gc)("Unthrottle " PTR_FORMAT " for words: " SIZE_FORMAT, p2i(current_thread), words);
#endif
  intptr_t old_budget, new_budget;
  do {
    old_budget = Atomic::load(&_available_words);
    new_budget = old_budget + words;
  } while (Atomic::cmpxchg(&_available_words, old_budget, new_budget, memory_order_relaxed) != old_budget);

  // Notify unconditionally.  Don't second guess whether there are threads waiting in throttling requests. Threads
  // can be waiting even if original_budget was > 0 because very large requests may be forced to delay even when
  // budget is sufficient and smaller requests will delay if budget is positive but insufficient.
  wake_throttled();
}

intptr_t ShenandoahThrottler::epoch() {
  return Atomic::load(&_epoch);
}

// Return the number of words for which allocation has been authorized.  If this is a TLAB request and we are configured
// for elastic TLABS and we cannot immediately claim authorization for the request, downsize to 1/8 requested size, or
// minimum TLAB size, whichever is larger.

// In case we cannot get authorization for words after waiting through an entire epoch, we force the authorization.
// Of course, the forced authorization may result in an allocation failure, which may ultimately degenerate.
size_t ShenandoahThrottler::throttle_for_alloc(ShenandoahAllocRequest req) {
  assert(ShenandoahThrottleAllocations, "Only be here when pacing is enabled");
  size_t words = req.size();
  // Fast path: try to allocate right away
  if ( claim_for_alloc(words, false)) {
    return words;
  }

  // Threads that are attaching should not block at all: they are not fully initialized yet. Blocking them would be awkward.
  // This is probably the path that allocates the thread oop itself.
  //
  // Thread which is not an active Java thread should also not block. This can happen during VM init when main thread is
  // still not an active Java thread.
  JavaThread* current_thread = JavaThread::current();
  if (current_thread->is_attaching_via_jni() || !current_thread->is_active_Java_thread()) {
    if (req.is_lab_alloc() && ShenandoahElasticTLAB) {
      // Since we're in a throttling situation, make our TLAB request more conservative
      words = req.min_size();
    }
    bool claimed = claim_for_alloc(words, true);
    assert(claimed, "Forceful claim should always succeed");
    return words;
  }

  // ShenandoahPacer forces a FIFO ordering of requests.  It claims authorization for allocate immediately
  // and then it stalls until the authorization budget is no longer negative.  Some issues with that approach:
  //
  //  1. It penalizes all threads equally.  The light allocators pay the same prices as the heavy allocators.
  //  2. The light allocators may be blocked in their progress because a heavy allocator inserted its request ahead of theirs.
  //  3. Once we have deficit spending in the budget, all allocating threads must wait for the deficit to be resolved.
  //     There is no mechanism to say "my deficit has been resolved so I should be allowed to move on."
  //
  // On the other hand, ShenandoahPacer limits the total time that any thread will stall on each allocation request,
  // except that this limit is only artificial, because if ShenandoahPacer releases a thread before it's allocation
  // deficit has been paid, we are very likely to end up in degeneration, and then everyone pays much higher stall costs.
  //
  // The mechanism implemented by ShenandoahThrottleAllocation may be vulnerable to priority inversion.  We prioritize
  // allocations to the threads that have lower allocation needs, and these may not be the most "important" threads.
  //
  // But this is inherent in Java.  There is no notion of "importance" or "urgency" represented in thread state.
  //
  // Thus, we bias for "productivity".  If there are any threads that can do productive work within the context of our
  // current shortfall of available memory, we let these threads run.  To the extent that these threads get work done now,
  // this will reduce contention with the other threads that are currently waiting for availability of memory.

  double start_time = os::elapsedTime();
  intptr_t epoch_at_start = Atomic::load(&_epoch);
  intptr_t current_epoch;
  size_t cumulative_pause = 0;
  size_t current_pause = MIN_THROTTLE_DELAY_MS;


#ifdef KELVIN_THROTTLE
  size_t available =  Atomic::load(&_available_words);
  log_info(gc)("Throttle " PTR_FORMAT " for %s req, size: " SIZE_FORMAT " out of " SIZE_FORMAT " (epoch: %ld)",
               p2i(current_thread), req.is_lab_alloc()? "tlab": "shared", req.size(), available, epoch_at_start);
#endif
  words = req.size();
  do {
    wait(current_pause);
    if (req.is_lab_alloc() && ShenandoahElasticTLAB) {
      words /= 4;
      if (words < req.min_size()) {
        words = req.min_size();
      }
    }
    // Since we've waited at least current_pause ms, we can allow greed in large allocation requests
    bool claimed = claim_for_alloc(words, false, true);
    if (claimed) {
      double end_time = os::elapsedTime();
      double throttle_time = end_time - start_time;

      // Since ShenandoahPacing is mutually exclusive with ShenandoahThrottleAllocations, reuse this field of
      // ShenandoahThreadLocalData
      ShenandoahThreadLocalData::add_paced_time(current_thread, throttle_time);
      add_to_metrics(true, words, throttle_time);
#ifdef KELVIN_THROTTLE
      size_t new_available =  Atomic::load(&_available_words);
      log_info(gc)("Release thread " PTR_FORMAT ", authorized " SIZE_FORMAT " after delay: %.6f with remnant available: " SIZE_FORMAT,
                   p2i(current_thread), words, throttle_time, new_available);
#endif
      return words;
    }
    current_pause *= 2;
    if (current_pause > MAX_THROTTLE_DELAY_MS) {
      current_pause = MAX_THROTTLE_DELAY_MS;
    }
    current_epoch = Atomic::load(&_epoch);
  } while (current_epoch - epoch_at_start <= 2);

  // Force the authorization, which may result in allocation failure and degeneration.
  claim_for_alloc(words, true);
  double end_time = os::elapsedTime();
  double throttle_time = end_time - start_time;

  // Since ShenandoahPacing is mutually exclusive with ShenandoahThrottleAllocations, reuse this field of
  // ShenandoahThreadLocalData
  ShenandoahThreadLocalData::add_paced_time(current_thread, throttle_time);
  add_to_metrics(false, words, throttle_time);
#ifdef KELVIN_THROTTLE
      log_info(gc)("Forced release throttle for thread " PTR_FORMAT ", allocating " SIZE_FORMAT " after throttle_time: %.6f",
                   p2i(current_thread), words, throttle_time);
#endif
  return words;
}

void ShenandoahThrottler::wait(size_t time_ms) {
  // Perform timed wait. It works like like sleep(), except without modifying
  // the thread interruptible status. MonitorLocker also checks for safepoints.
  assert(time_ms > 0, "Should not call this with zero argument, as it would stall until notify");
  assert(time_ms <= LONG_MAX, "Sanity");
//   Atomic::inc(&_threads_in_throttle, memory_order_relaxed);
  MonitorLocker locker(_wait_monitor);
  _wait_monitor->wait((long)time_ms);
//  Atomic::dec(&_threads_in_throttle, memory_order_relaxed);
}

void ShenandoahThrottler::notify_waiters() {
  if (_need_notify_waiters.try_unset()) {
    MonitorLocker locker(_wait_monitor);
    _wait_monitor->notify_all();
  }
}

#ifdef KELVIN_DEPRECATE
// Seems too expensive to iterate over all mutator threads at the end of every GC cycle, especially since
// we intend for thottling events to be very rare.
// TBD whether I want to keep this level of reporting.
// Compromise: might tally all of this information only at program termination.

void ShenandoahThrottler::flush_stats_to_cycle() {
  double sum = 0;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    sum += ShenandoahThreadLocalData::paced_time(t);
  }
  ShenandoahHeap::heap()->phase_timings()->record_phase_time(ShenandoahPhaseTimings::pacing, sum);
}

void ShenandoahThrottler::print_cycle_on(outputStream* out) {
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
#endif
