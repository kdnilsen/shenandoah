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
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/threadSMR.hpp"

#undef KELVIN_VERBOSE
#ifdef KELVIN_VERBOSE
size_t _pacer_total_mark = 0;
size_t _pacer_total_evac = 0;
size_t _pacer_total_update = 0;
size_t _pacer_total_alloc = 0;

void pacer_reset_tallies() {
  _pacer_total_mark = 0;
  _pacer_total_evac = 0;
  _pacer_total_update = 0;
  _pacer_total_alloc = 0;
}

#endif


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

#ifndef FUBAR
#define _is_generational _heap->mode()->is_generational()
#else
bool ShenandoahPacer::is_generational() {
  return _heap->mode()->is_generational();
}
#endif

void ShenandoahPacer::setup_for_mark() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  // During marking, work progress is represented by total words marked.  Accumulation of marked words during marking
  // is not linear.  During the initial stages of marking, almost every object seen has not yet been marked.  During
  // later stages of marking, the large majority of objects seen have already been marked, so accumulation of work
  // progress slows.  On the other hand, we are generally conservative in our estimate of how much total work needs to
  // be accomplished, so the marking effort will usually complete before the slower progress at the end of this phase
  // causes pacing to aggressively slow mutator allocations.
  //
  // Bottom line: our configuration of pacing during concurrent mark favors greedy behavior.  We strive not to stall
  // allocators unless we are in "dire straits".

  if (_is_generational)
  {
#ifdef KELVIN_VERBOSE
    extern void pacer_reset_tallies();
    pacer_reset_tallies();
#endif

    size_t live_bytes = Atomic::load(&_most_recent_young_live_bytes);
    size_t young_used = _heap->young_generation()->used();
    if (live_bytes == 0) {
      // No history yet.  Assume everything needs to be marked. During initialization, a high percentage of objects will
      // be persistent, so this is a safe conservative assumption.  Note that we do not have to "mark" objects that are
      // above TAMS.
      live_bytes = young_used / 2;
    }

    // Conservatively estimate that we'll need to mark twice as much memory as was live at previous mark.
    size_t projected_work = MIN2(live_bytes * 2, young_used);

    // Allow ourselves to allocate 3/4 of remaining available during concurrent mark, recognizing that concurrent
    // marking is typically the most time consuming phase of GC (e.g. 70%).  Recognize that any immediate trash
    // reclaimed at the end of concurrent marking will allow us to expand the budget when we begin concurrent evacuation.
    // In other words, allowing 75% of available to be allocated during concurrent marking is probably conservative.
    size_t young_available = _heap->young_generation()->available();
    intptr_t allocation_budget = (intptr_t) ((3 * young_available) / 4);
    intptr_t allocation_ratio = allocation_budget * 1024 / projected_work;

    Atomic::store(&_allocation_per_work_ratio_per_K, allocation_ratio);
    Atomic::store(&_preauthorization_debt, allocation_budget / 2);
    Atomic::store(&_incremental_phase_work_completed, 0L);
    Atomic::store(&_incremental_allocation_budget, allocation_budget / 2);

#ifdef KELVIN_VERBOSE
    log_info(gc, ergo)("Mark pace calculations, young used: " SIZE_FORMAT ", live_bytes: " SIZE_FORMAT, young_used, live_bytes);
    log_info(gc, ergo)(" more, available: " SIZE_FORMAT " alloc_budget: " SIZE_FORMAT ", anticipated work: " SIZE_FORMAT,
                       young_available, allocation_budget, projected_work);
#endif
    log_info(gc, ergo)("Pacer for Mark: Expected Work: " SIZE_FORMAT "%s, Phase Allocation Budget: "
                       SIZE_FORMAT "%s, Alloc/Work Ratio (per K): " SIZE_FORMAT,
                       byte_size_in_proper_unit(projected_work), proper_unit_for_byte_size(projected_work),
                       byte_size_in_proper_unit((size_t) allocation_budget),
                       proper_unit_for_byte_size((size_t) allocation_budget), allocation_ratio);
  } else {
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
}

void ShenandoahPacer::setup_for_evac(size_t immediate_trash_used, size_t evacuation_bytes) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahCollectionSet* collection_set = heap->collection_set();

  assert(ShenandoahPacing, "Only be here when pacing is enabled");
  assert(collection_set != nullptr, "Collection set must be non-NULL to pace evacuation");

  if (_is_generational) {
    size_t heap_usage = heap->used();
    size_t collection_set_usage = collection_set->used();

#ifdef KELVIN_VERBOSE
    extern void pacer_reset_tallies();
    pacer_reset_tallies();
#endif

    // Recompute available.  Though pacing during mark prevents us from over-allocating within available, we may have earned
    // an unexpected benefit if immediate trash was found and recycled at end of concurrent marking.  Also, if allocation
    // rate was slower than GC rate, we may have more than the planned amount of memory available right now.
    //
    // Note that immediate_trash_used will soon be added into available and memory required to represent evacuated
    // objects is not available for mutator allocations.
    size_t available = _heap->young_generation()->available() + immediate_trash_used - evacuation_bytes;

    // How much memory needs to be evacuated?  This is "used" within the selected collection set.  Note that the work
    // of evacuation actually includes only the amount of live rather than the amount used.  But the report_evac()
    // totals are provided in terms of used.
    size_t evacuation_workload = collection_set_usage;

    // How much memory needs to be updated?  This is:
    //   1. used within old-gen (conservatively we'll need to process all used within every old heap region)
    //   2. plus used within young (all used within young at this moment in time is below update_watermark)
    //   3. minus used within the young collection set (the collection set does not need to be updated)
    //   4  plus the evacuation workload (all evacuated objects must be updated)
    size_t update_refs_workload = heap_usage - collection_set_usage - immediate_trash_used + evacuation_bytes;

    // Remember how much memory will have to be processed by update-refs, since this will guide pacing of update-refs.
    _usage_below_update_watermark = update_refs_workload;

    // Remember how much memory was marked by young-gen concurrent marking.  This is memory below TAMS, aka live at start
    // of young-gen concurrent marking with SATB barrier.  This can be calculated as total young usage minus memory
    // allocated since start of young-gen GC.
    _most_recent_young_live_bytes = (_heap->young_generation()->heuristics()->get_live_bytes()
                                     - _heap->young_generation()->bytes_allocated_since_gc_start());
    // evacuation needs to read and write every word
    // update-refs needs to read every pointer field and if the reference resides in collection-set (from-space), find the
    // forwarding pointer and then overwrite the original word.  This is more work on fewer numbers of values.  We don't have
    // a good mechanism to predict the percentage of pointers to be contained within the memory ranges that needs to have
    // references updated.

    // Current approximation: divide the remaining allocations in proportion to the sizes of the memory ranges that have
    // to be processed by each.

    float percent_evac = ((float) evacuation_workload) / ((float) (evacuation_workload + update_refs_workload));

    // If we are planning to spend 15% of remaining GC effort in evacuation, then we will allow 15% of remaining
    // available memory to be allocated during evacuation.
    size_t evac_allocation_budget = (size_t) (available * percent_evac);
    size_t allocation_ratio = evac_allocation_budget * 1024 / evacuation_workload;

    Atomic::store(&_allocation_per_work_ratio_per_K, (intptr_t) allocation_ratio);
    Atomic::store(&_preauthorization_debt, (intptr_t) evac_allocation_budget / 2);
    Atomic::store(&_incremental_phase_work_completed, 0L);
    Atomic::store(&_incremental_allocation_budget, (intptr_t) evac_allocation_budget / 2);
#ifdef KELVIN_VERBOSE
    log_info(gc, ergo)("Evac pace calculations (1 of 5)"
                       ", heap used: " SIZE_FORMAT
                       ", collection set used: "  SIZE_FORMAT,
                       heap_usage, collection_set_usage);
    log_info(gc, ergo)("Evac pace calculations (2 of 5)"
                       ", evac workload: "  SIZE_FORMAT
                       ", update refs workload: "  SIZE_FORMAT,
                       evacuation_workload, update_refs_workload);
    log_info(gc, ergo)("Evac pace calculations (3 of 5)"
                       ", immediate trash: " SIZE_FORMAT
                       ", evacuation reserve: " SIZE_FORMAT
                       ", alloc budget dedicated to evac: %0.2f%%",
                       immediate_trash_used, evacuation_bytes, percent_evac * 100);
    log_info(gc, ergo)("Evac pace calculations (4 of 5)"
                       ", young available: " SIZE_FORMAT
                       ", divided between evac budget: "  SIZE_FORMAT,
                       available, evac_allocation_budget);
    log_info(gc, ergo)("Evac pace calculations (5 of 5)"
                       ", and update-refs budget:  " SIZE_FORMAT
                       ", most recent young live bytes: "  SIZE_FORMAT
                       ", bytes allocated during mark: "  SIZE_FORMAT,
                       available - evac_allocation_budget, _most_recent_young_live_bytes,
                       _heap->young_generation()->bytes_allocated_since_gc_start());
    size_t num_regions = _heap->num_regions();
    size_t total_update_work = 0;
    for (size_t i = 0; i < num_regions; i++) {
      ShenandoahHeapRegion* r = _heap->get_region(i);
      log_info(gc, ergo)("%s Region " SIZE_FORMAT ": %s %s %s bottom: " PTR_FORMAT ", update_watermark: " PTR_FORMAT,
                         affiliation_name(r->affiliation()), r->index(),
                         r->is_active()? "active": "inactive",
                         r->is_cset()? "(cset)": "(not cset)",
                         r->is_trash()? "(garbage)": "(gold)",
                         p2i(r->bottom()), p2i(r->get_update_watermark()));
      if (r->is_active() && !r->is_cset() && !r->is_trash()) {
        total_update_work += (r->get_update_watermark() - r->bottom());
        log_info(gc, ergo)("  Adding update work " PTR_FORMAT " to total updateref work: " PTR_FORMAT,
                           r->get_update_watermark() - r->bottom(), total_update_work);
      }
    }
    total_update_work *= HeapWordSize;
    log_info(gc, ergo)("Computed update-refs work is: " SIZE_FORMAT, total_update_work);
#endif
    log_info(gc, ergo)("Pacer for Evacuation: Expected Work: " SIZE_FORMAT "%s, Phase Allocation Budget: "
                       SIZE_FORMAT "%s, Alloc/Work Ratio (per K): " SIZE_FORMAT,
                       byte_size_in_proper_unit(evacuation_workload), proper_unit_for_byte_size(evacuation_workload),
                       byte_size_in_proper_unit(evac_allocation_budget), proper_unit_for_byte_size(evac_allocation_budget),
                       allocation_ratio);
  } else {
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
}

void ShenandoahPacer::setup_for_updaterefs() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahCollectionSet* collection_set = heap->collection_set();

  assert(ShenandoahPacing, "Only be here when pacing is enabled");
  assert(collection_set != nullptr, "Collection set must be non-NULL to pace update refs");

  if (_is_generational) {
#ifdef KELVIN_VERBOSE
    extern void pacer_reset_tallies();
    pacer_reset_tallies();
#endif

    // Recompute available.  Let's see how much of the previously available budget was consumed during evacuation.
    size_t update_refs_allocation_budget = _heap->young_generation()->available();
    size_t update_refs_workload = _usage_below_update_watermark;
    size_t allocation_ratio = update_refs_allocation_budget * 1024 / update_refs_workload;

    // Evac runs fast because collection set is often very sparse.  Furthermore, evac front-loads allocations because
    // TLABs are refreshed at start of evac (in order to maintain update_watermark.

    Atomic::store(&_allocation_per_work_ratio_per_K, (intptr_t) allocation_ratio);
    Atomic::store(&_preauthorization_debt, (intptr_t) update_refs_allocation_budget / 4);
    Atomic::store(&_incremental_phase_work_completed, 0L);
    Atomic::store(&_incremental_allocation_budget, (intptr_t) update_refs_allocation_budget / 4);

#ifdef KELVIN_VERBOSE
    log_info(gc, ergo)("Update-refs pacing calculations (1/2)"
                       ", available: " SIZE_FORMAT
                       ", update_refs_workload: "  SIZE_FORMAT,
                       update_refs_allocation_budget, update_refs_workload);
    log_info(gc, ergo)(" more calculations (2/2)"
                       ", preauthorized: " SIZE_FORMAT, update_refs_allocation_budget / 4);
    size_t num_regions = _heap->num_regions();
    size_t total_update_work = 0;
    for (size_t i = 0; i < num_regions; i++) {
      ShenandoahHeapRegion* r = _heap->get_region(i);
      log_info(gc, ergo)("%s Region " SIZE_FORMAT ": %s %s %s bottom: " PTR_FORMAT ", update_watermark: " PTR_FORMAT,
                         affiliation_name(r->affiliation()), r->index(),
                         r->is_active()? "active": "inactive",
                         r->is_cset()? "(cset)": "(not cset)",
                         r->is_trash()? "(garbage)": "(gold)",
                         p2i(r->bottom()), p2i(r->get_update_watermark()));
      if (r->is_active() && !r->is_cset() && !r->is_trash()) {
        total_update_work += (r->get_update_watermark() - r->bottom());
        log_info(gc, ergo)("  Adding update work " PTR_FORMAT " to total updateref work: " PTR_FORMAT,
                           r->get_update_watermark() - r->bottom(), total_update_work);
      }
    }
    total_update_work *= HeapWordSize;
    log_info(gc, ergo)("Computed update-refs work is: " SIZE_FORMAT, total_update_work);

#endif
    log_info(gc, ergo)("Pacer for Updating Refs: Expected Work: " SIZE_FORMAT "%s, Phase Allocation Budget: "
                       SIZE_FORMAT "%s, Alloc/Work Ratio (per K): " SIZE_FORMAT,
                       byte_size_in_proper_unit(update_refs_workload), proper_unit_for_byte_size(update_refs_workload),
                       byte_size_in_proper_unit(update_refs_allocation_budget),
                       proper_unit_for_byte_size(update_refs_allocation_budget), allocation_ratio);
  } else {
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
}

/*
 * In idle phase, we have to pace the application to let control thread react with GC start.
 *
 * Here, we rendezvous with concurrent thread that adds up the budget as it acknowledges
 * it had seen recent allocations. It will naturally pace the allocations if control thread is
 * not catching up. To bootstrap this feedback cycle, we need to start with some initial budget
 * for applications to allocate at.
 */

void ShenandoahPacer::setup_for_idle() {
  ShenandoahCollectionSet* collection_set = _heap->collection_set();

  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  if (_is_generational) {
#ifdef KELVIN_VERBOSE
    extern void pacer_reset_tallies();
    pacer_reset_tallies();
#endif
    if (_heap->is_concurrent_old_mark_in_progress()) {
      if (_concurrent_old_interruption_count++ == 0) {
        // This is the first increment of old-gen work on concurrent marking.

        size_t old_used = _heap->old_generation()->used();
        if (_most_recent_old_live_bytes == 0) {
          // The first time we do old-gen collection, we're flying blind.  Conservatively, plan to mark everything.
          _most_recent_old_live_bytes = old_used / 2;
        }

        // This is the first of potentially many increments of old-gen concurrent marking efforts.
        // TODO: Our current approach is to "arbitrarily" reserve 3/4 planned old-gen increments for concurrent
        // marking and 1/4 for concurrent preparation for mixed evacuations.  Would be better to be adaptive here,
        // based on measurement of recent history.
        _planned_old_marking_passes = (3 * ShenandoahPacingOldRatio) / 4;

        // Use a negative value to indicate that we haven't yet started preparation for mixed evacuation.
        _planned_old_mixed_evac_prep_passes = -(ShenandoahPacingOldRatio - _planned_old_marking_passes);
        if (_planned_old_mixed_evac_prep_passes == 0) {
          _planned_old_mixed_evac_prep_passes = -1; // Assure at least one increment of work to prep for mixed evac.
        }

        // Estimate the upper bound to be twice the amount of live memory detected by the previous old-gen concurrent
        // mark pass, but no more than old_generation->used();  Since we are normally running with old-gen at utilization
        // above 50%, the _total_planned_old_marking_effort will typically equal old_used.
        _total_planned_old_effort = MIN2(_most_recent_old_live_bytes * 2, old_used);

        // Our estimate is "conservative".  Normally, old-gen marking will complete will ahead of the planned number
        // of old marking passes, because the amount of live memory is typically much less than _total_planned_old_marking_effort.
        // In case we need to use all of the conservatively planned increments, the pacer will slow down the last cycle by
        // requiring that the last increment of marking perform enough work to completely mark all of old_used.  In the
        // rare event that _total_planned_old_marking_effort is less than old_used, add the extra effort required to mark
        // all of old used during the last planned increment of old-gen concurrent marking.
        _last_mark_increment_supplement = old_used - _total_planned_old_effort;

      }
      size_t increment_effort;
      if (_planned_old_marking_passes-- > 0) {
        if (_planned_old_marking_passes == 0) {
          // This is the last planned increment of concurrent marking
          increment_effort = _total_planned_old_effort + _last_mark_increment_supplement;
          // This is last increment of concurrent marking.  Then, we'll overwrite _total_planned_old_effort below.
        } else {
          increment_effort = _total_planned_old_effort / (_planned_old_marking_passes + 1);
          _total_planned_old_effort -= increment_effort;  // Adjust how much old effort remains.
        }
      }
      size_t young_available = _heap->young_generation()->available();
      size_t young_trigger = _heap->young_generation()->heuristics()->start_gc_threshold();
      size_t allocation_budget;
      if (young_available > young_trigger) {
        allocation_budget = young_available - young_trigger;
      } else {
        allocation_budget = 0;       // Don't allow any allocation during this old-gen increment!
      }

      // Since idle phases are not front-loaded with allocations, we preauthorize a smaller fraction of total
      // allocation buffer.  This reduces the likelihood that we will experience allocation stalls near the
      // end of this idle phase.
      size_t allocation_ratio = allocation_budget * 1024 / increment_effort;
      Atomic::store(&_allocation_per_work_ratio_per_K, (intptr_t) allocation_ratio);
      Atomic::store(&_preauthorization_debt, (intptr_t) allocation_budget / 8);
      Atomic::store(&_incremental_phase_work_completed, 0L);
      Atomic::store(&_incremental_allocation_budget, (intptr_t) allocation_budget / 8);

#define KELVIN_VERBOSE
#ifdef KELVIN_VERBOSE
      printf("ShenandoahPacer::generational setup_for_idle concurrent marking, anticipated effort: " SIZE_FORMAT
             ", budget: " SIZE_FORMAT "\n", increment_effort, allocation_budget);
#endif
      log_info(gc, ergo)("Pacer for Idle span: Expected Concurrent Old-Gen Marking Work: " SIZE_FORMAT 
                         "%s, Phase Allocation Budget: " SIZE_FORMAT "%s",
                         byte_size_in_proper_unit(increment_effort), proper_unit_for_byte_size(increment_effort),
                         byte_size_in_proper_unit(allocation_budget), proper_unit_for_byte_size(allocation_budget));
      log_debug(gc)("Allocate/Work ratio during Concurrent Marking per_K is " SIZE_FORMAT, allocation_ratio);
    }
#ifdef KELVIN_DEPRECATE
    // AT TIME OF PRIOR IMPLEMENTATION, COALESCE AND FILL WOULD HAPPEN
    // AFTER END OF OLD-GEN MARKING EFFORT.  WE WOULD TRACK PROGRESS
    // OF THE COALESCE-AND-FILL EFFORT HERE.  AS CURRENTLY
    // IMPLEMENTED, I BELIEVE WE LEAVE THE COALE

    else if (_heap->is_concurrent_prep_for_mixed_evacuation_in_progress()) {
      assert(!_heap->is_concurrent_old_mark_in_progress(), "Finish old-gen marking before preparationfor mixed evacuation");
      if (_planned_old_mixed_evac_prep_passes < 0) {
        // This is the first of potentially several preparation for mixed evacuation increments of work.
        if (_planned_old_marking_passes > 0) {
          // If we finished concurrent marking ahead of schedule, relax pacing constraints on prep for mixed evacuation.
          _planned_old_mixed_evac_prep_passes += _planned_old_marking_passes;
        }
        _total_planned_old_effort = _heap->old_generation()->used() - _heap->old_heuristics()->old_collection_candidates_used();
      }
      size_t increment_effort = _total_planned_old_effort / _planned_old_mixed_evac_prep_passes;
      _planned_old_mixed_evac_prep_passes -= 1;
      _total_planned_old_effort -= increment_effort;
      size_t young_available = _heap->young_generation()->available();
      size_t young_trigger = _heap->young_generation()->heuristics()->start_gc_threshold();
      size_t allocation_budget;
      if (young_available > young_trigger) {
        allocation_budget = young_available - young_trigger;
      } else {
        allocation_budget = 0;       // If we're "wanting" to schedule back-to-back young-gen collections, then we'll
                                     // have to stall all mutator threads and defer all alloctions until we've completed
                                     // the requisite amount of old-gen effort.
      }

      // Since idle phases are not front-loaded with allocations, we preauthorize a smaller fraction of total
      // allocation buffer.  This reduces the likelihood that we will experience allocation stalls near the
      // end of this idle phase.
      size_t allocation_ratio = allocation_budget * 1024 / increment_effort;
      Atomic::store(&_allocation_per_work_ratio_per_K, (intptr_t) allocation_ratio);
      Atomic::store(&_preauthorization_debt, (intptr_t) allocation_budget / 8);
      Atomic::store(&_incremental_phase_work_completed, 0L);
      Atomic::store(&_incremental_allocation_budget, (intptr_t) allocation_budget / 8);
#define KELVIN_VERBOSE
#ifdef KELVIN_VERBOSE
      printf("ShenandoahPacer::generational setup_for_idle prep for mixed evacuations, anticipated effort: " SIZE_FORMAT ", budget: " SIZE_FORMAT "\n",
             increment_effort, allocation_budget);
#endif
      log_info(gc, ergo)("Pacer for Idle span with Concurrent Old-Gen Mixed Evac Prep Work: " SIZE_FORMAT
                         "%s, Phase Allocation Budget: " SIZE_FORMAT "%s, Alloc/Work Ratio (per K): " SIZE_FORMAT,
                         byte_size_in_proper_unit(increment_effort), proper_unit_for_byte_size(increment_effort),
                         byte_size_in_proper_unit(allocation_budget), proper_unit_for_byte_size(allocation_budget),
                         allocation_ratio);
      log_debug(gc)("Allocate/Work ratio during Mixed Evac Prep per_K is " SIZE_FORMAT, allocation_ratio);
    }
#endif  // KELVIN_DEPRECATE
    else {
      // Set _incremental_phase_work_completed to -1, effectively disabling pacing.
      Atomic::store(&_incremental_phase_work_completed, -1L);
    }
  } else {
    size_t initial = _heap->max_capacity() / 100 * ShenandoahPacingIdleSlack;
    double tax = 1;

    restart_with(initial, tax);
    log_info(gc, ergo)("Pacer for Idle. Initial: " SIZE_FORMAT "%s, Alloc Tax Rate: %.1fx",
                       byte_size_in_proper_unit(initial), proper_unit_for_byte_size(initial), tax);
  }
}

/*
 * There is no useful notion of progress for these operations. To avoid stalling
 * the allocators unnecessarily, allow them to run unimpeded.
 */

void ShenandoahPacer::setup_for_reset() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  if (_is_generational) {
#ifdef KELVIN_VERBOSE
    extern void pacer_reset_tallies();
    pacer_reset_tallies();
#endif
    // Disable pacing.
    Atomic::store(&_incremental_phase_work_completed, -1L);
    log_info(gc, ergo)("Pacer for Generational Reset: pacing is disabled");
  } else {
    size_t initial = _heap->max_capacity();
    restart_with(initial, 1.0);
    log_info(gc, ergo)("Pacer for Reset. Non-Taxable: " SIZE_FORMAT "%s",
                       byte_size_in_proper_unit(initial), proper_unit_for_byte_size(initial));
  }
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
  assert(!_is_generational, "Only be here when not generational");

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

// Account for allocations that cannot be avoided and cannot be blocked, such as when an attaching JNI thread needs to
// allocate memory and when a GC thread creates waste or alignment padding within young-gen memory.
void ShenandoahPacer::force_generational_claim_for_alloc(size_t words) {

  size_t bytes = words * HeapWordSize;
#ifdef KELVIN_VERBOSE
  log_info(gc, ergo)("force_generational_claim_for_alloc(" SIZE_FORMAT " bytes)", bytes);
#endif
  // Don't bother to account for recently completed work.  It won't impact whether or not this allocation is "approved".
  intptr_t allocate_budget, new_allocate_budget;
  do {
    allocate_budget = Atomic::load(&_incremental_allocation_budget);
    new_allocate_budget = allocate_budget - bytes;
  } while (Atomic::cmpxchg(&_incremental_allocation_budget, allocate_budget, new_allocate_budget, memory_order_relaxed)
           != allocate_budget);
}

// Once we exceed the allocation budget, it may take a relatively long time for GC progress to catch up with allocation
// requests and this will cause a waiting allocator to pause multiple times.  The total stall time has been observed to
// require over a second in common scenarios.  By checking whether a generational claim to memory is at risk before 
// attempting to claim the allocation, we allow the stall cost to be divided between multiple threads, and we break a
// single potentially very long stall into multiple shorter stalls.
bool ShenandoahPacer::is_generational_claim_at_risk(size_t words) {
  // TODO: May want to make ProximityThresholdMultiplier a dynamic value.  Here's one way to do this:
  //
  //  1. In pace_for_alloc(words), use a static hash table with HashTableSize = 257 entries to represent the unique
  //     threads that have accumulated stalls during the current GC pass.
  //  2. The key values stored in this hash table are JavaThread::current() | (epoch() & 0x03)
  //  3. At start of GC, set the _threads_stalled_by_pacing_count variable to zero.
  //  4. When the pacer decides to stall an allocating thread, it atomically increments _threads_stalled_by_pacing_count.
  //     If this is the first thread to increment this counter during the current GC pass, this thread zeros out the
  //     contents of the hash table before it implements it sleep operation.
  //  5. After a waiting thread wakes up, it adds itself to the hash table of stalling threads using closed hashing but
  //     only if _threads_stalled_by_pacing_count <= (HashTableSize / 2) .  If 
  //     We use atomic loads to detect whether a hash slot is open.  We use atomic stores to overwrite a particular
  //     hash slot.
  //  6. If this thread could not be added to the hash table because it was already present in the hash table, or because
  //     the maximum size of the hash table has been reached, atomically decrement _threads_stalled_by_pacing_count, but
  //     do not decrement below 1.  The "floor(1)" filter addresses the scenario in which a thread is put to sleep during
  //     epoch N and wakes up during epoch N+1.
  //  7. At the end of each gc pass, copy the value of _threads_stalled_by_pacing_count to
  //     _threads_stalled_by_pacing_count_in_previous_epoch.
  //  8. Let ProximityThresholdMultiplier equal
  //       MAX2(_threads_stalled_by_pacing_count, _threads_stalled_by_pacing_count_in_previous_epoch)
  //
  // General ideas:
  //
  //  The risk of overflowing the allocation budget is proportional to the number of threads that are doing "heavy"
  //    allocation.
  //  An approximate count of these threads allows risk to be approximated.
  //
  // Observation:
  //   If ProximityThresholdMultiplier is too large, we trigger stalls even when we're nowhere close to exahusting supply
  //   of memory.  But the precautionary stalls are typically short (ShenandoahPacingMaxDelay), and no particular thread
  //   will experience very many of these unless that thread is frequently allocating.  If ProximityThreshold is too small,
  //   we eventually step over the edge of the cliff and need to stall until GC finishes its current phase of effort, which
  //   may cause this particular thread to stall longer than a full GC because concurrent GC does not finish as quickly as
  //   full GC.
  //
  //   Suppose a single thread is making equal-sized allocations.  In the worst case, this thread will experience
  //   ProximityThresholdMultiplier stalls of length ShenandoahPacingMaxDelay as it approaches the end of its allocation
  //   budget.  In "better" cases, GC makes sufficient progress between consecutive alloation requests that it does
  //   not have to continue stalling.  Threads that allocate smaller chunks or perform less frequent allocations will
  //   be much less likely to experience significant allocation stalls.

  const int ProximityThresholdMultiplier = 10;
  size_t bytes = words * HeapWordSize;

  intptr_t phase_work = Atomic::load(&_incremental_phase_work_completed);
  if (phase_work < 0) {
    return false;               // Pacing is disabled so no risk.
  } else {
    intptr_t allocate_budget =  Atomic::load(&_incremental_allocation_budget);
    if ((size_t) allocate_budget > bytes * ProximityThresholdMultiplier) {
      return false;             // Existing budget is sufficient, so no risk.
    } else if (phase_work > 0) {
      size_t allocate_ratio = Atomic::load(&_allocation_per_work_ratio_per_K);
      // Consider payments on preauthorization to be neglible, so ignore for this calculation.
      if ((size_t) (allocate_budget + (phase_work * allocate_ratio / 1024)) > bytes * ProximityThresholdMultiplier) {
        return false;           // Existing budget plus recent work is sufficient, so no risk.
      }
    }
  }
  return true;
}

bool ShenandoahPacer::claim_for_generational_alloc(size_t words) {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");
  assert(_is_generational, "Only be here when is generational");

#ifdef KELVIN_VERBOSE
  size_t _work_credit, _budget_increment;
  intptr_t _original_debt, _original_budget, _original_work, _debt_payment;
  intptr_t _new_debt, _new_budget;
#endif

  size_t bytes = words * HeapWordSize;
  // Try to claim credit for recently completed work.
  intptr_t phase_work;
  do {
    phase_work = Atomic::load(&_incremental_phase_work_completed);
#ifdef KELVIN_VERBOSE
    _original_work = phase_work;
    _work_credit = 0;           // overwrite below if _original_work > 0
#endif
    if (phase_work < 0) {
#ifdef KELVIN_VERBOSE
      log_info(gc, ergo)("claim_generational success (pacing disabled because phase_work is %ld)",
                         phase_work);
#endif
      return true;              // Pacing is disabled, so we don't need to claim for alloc
    } else if (phase_work == 0) {
      break;
    }
  } while (Atomic::cmpxchg(&_incremental_phase_work_completed, phase_work, 0L, memory_order_relaxed) != phase_work);
  // I have taken responsibility for adjusting budget based on completion of reported work

#ifdef KELVIN_VERBOSE
  _work_credit = 0;
  _debt_payment = 0;
  _original_debt = 0;
  _new_debt = 0;
  _new_budget = 0;
#endif

  size_t allocate_delta;
  if (phase_work > 0) {
    // Since we claimed credit for recently completed work, we'll try to adjust the allocation budget to account both for
    // recently completed work and for my current allocation request.

    size_t allocate_ratio = Atomic::load(&_allocation_per_work_ratio_per_K);
    allocate_delta = (phase_work * allocate_ratio) / 1024;
    intptr_t preauthorization_payment = allocate_delta / 2;
#ifdef KELVIN_VERBOSE
    _work_credit = allocate_delta;
#endif
    // Pay down my debt.
    intptr_t preauthorized_debt, new_debt;
    do {
      preauthorized_debt = Atomic::load(&_preauthorization_debt);
#ifdef KELVIN_VERBOSE
      _original_debt = preauthorized_debt;
#endif
      if (preauthorized_debt <= 0) {
#ifdef KELVIN_VERBOSE
        log_info(gc, ergo)("Since preauthorized_debt <= 0, setting preauthorization_payment to 0");
#endif
        preauthorization_payment = 0;
        break;
      }

      if (preauthorization_payment > preauthorized_debt) {
        preauthorization_payment = preauthorized_debt;
      }
      new_debt = preauthorized_debt - preauthorization_payment;
#ifdef KELVIN_VERBOSE
      _debt_payment = preauthorization_payment;
      _new_debt = new_debt;  
#endif
    } while (Atomic::cmpxchg(&_preauthorization_debt, preauthorized_debt, new_debt, memory_order_relaxed) != preauthorized_debt);
    allocate_delta -= preauthorization_payment;
#ifdef KELVIN_VERBOSE
    _budget_increment = allocate_delta;
#endif
    
    // allocate_delta is the adjustment I'll make to allocation_budget (not yet including accounting for current allocation).
    if (allocate_delta >= bytes) {
      allocate_delta -= bytes;
#ifdef KELVIN_VERBOSE
      _budget_increment = allocate_delta;
#endif
      if (allocate_delta > 0) {
        // Increment budget so others can allocate. 
        intptr_t allocate_budget, new_allocate_budget;
        do {
          allocate_budget = Atomic::load(&_incremental_allocation_budget);
          new_allocate_budget = allocate_budget + allocate_delta;
#ifdef KELVIN_VERBOSE
          _original_budget = allocate_budget;
          _new_budget = new_allocate_budget;
#endif
        } while (Atomic::cmpxchg(&_incremental_allocation_budget, allocate_budget, new_allocate_budget, memory_order_relaxed)
                 != allocate_budget);
      }
#ifdef KELVIN_VERBOSE
      log_info(gc, ergo)("claim_generational success ("
                         "Original Budget: %ld"
                         ", Original Debt: %ld"
                         ", Work Completed: %ld"
                         ", Work Credit: " SIZE_FORMAT
                         ", Debt Payment: %ld"
                         ", New Debt: %ld"
                         ", Budget Increase: " SIZE_FORMAT
                         ", New Budget: " SIZE_FORMAT
                         ", Claimed Alloc: " SIZE_FORMAT,
                         _original_budget, _original_debt, _original_work, _work_credit, _debt_payment, _new_debt,
                         _budget_increment, _new_budget, bytes);
#endif
      return true;
    }
    // Else, we'll fall through to see if allocate_delta can be combined with existing budget to authorize
    // the requested allocation.
  } else {
    allocate_delta = 0;
#ifdef KELVIN_VERBOSE
    _budget_increment = 0;
#endif
  }

  // We know that allocate_delta < bytes.  It may equal 0.  See if allocate_delta + _allocate_budget > bytes
  intptr_t allocate_budget, new_allocate_budget;
  do {
    allocate_budget = Atomic::load(&_incremental_allocation_budget);
#ifdef KELVIN_VERBOSE
    _original_budget = allocate_budget;
#endif
    if (allocate_budget + allocate_delta < bytes) {
      if (allocate_delta == 0) {
        // We're over budget, so reject the allocation.
#ifdef KELVIN_VERBOSE
        log_info(gc, ergo)("claim_generational failure ("
                           "Original Budget: %ld"
                           ", Original Debt: %ld"
                           ", Work Completed: %ld"
                           ", Work Credit: " SIZE_FORMAT
                           ", Debt Payment: %ld"
                           ", New Debt: %ld"
                           ", Budget Increase: " SIZE_FORMAT
                           ", New Budget: " SIZE_FORMAT
                           ", Unclaimed Alloc: " SIZE_FORMAT,
                           _original_budget, _original_debt, _original_work, _work_credit, _debt_payment, _new_debt,
                           _budget_increment, _new_budget, bytes);
#endif
        return false;
      } else {                  // We'll reject the claim after we add allocate_delta into budget
        bool first_iteration = true;
        do {
          if (first_iteration) {
            first_iteration = false;
          } else {
            allocate_budget = Atomic::load(&_incremental_allocation_budget);
          }
          new_allocate_budget = allocate_budget + allocate_delta;
#ifdef KELVIN_VERBOSE
          _new_budget = new_allocate_budget;
#endif
        } while (Atomic::cmpxchg(&_incremental_allocation_budget, allocate_budget, new_allocate_budget, memory_order_relaxed)
                 != allocate_budget);
#ifdef KELVIN_VERBOSE
        log_info(gc, ergo)("claim_generational failure ("
                           "Original Budget: %ld"
                           ", Original Debt: %ld"
                           ", Work Completed: %ld"
                           ", Work Credit: " SIZE_FORMAT
                           ", Debt Payment: %ld"
                           ", New Debt: %ld"
                           ", Budget Increase: " SIZE_FORMAT
                           ", New Budget: " SIZE_FORMAT
                           ", Unclaimed Alloc: " SIZE_FORMAT,
                           _original_budget, _original_debt, _original_work, _work_credit, _debt_payment, _new_debt,
                           _budget_increment, _new_budget, bytes);
#endif
        return false;
      }
    }
    new_allocate_budget = allocate_budget + allocate_delta - bytes;
#ifdef KELVIN_VERBOSE
    _new_budget = new_allocate_budget;
#endif
  } while (Atomic::cmpxchg(&_incremental_allocation_budget, allocate_budget, new_allocate_budget, memory_order_relaxed)
           != allocate_budget);

#ifdef KELVIN_VERBOSE
  log_info(gc, ergo)("claim_generational success ("
                     "Original Budget: %ld"
                     ", Original Debt: %ld"
                     ", Work Completed: %ld"
                     ", Work Credit: " SIZE_FORMAT
                     ", Debt Payment: %ld"
                     ", New Debt: %ld"
                     ", Budget Increase: " SIZE_FORMAT
                     ", New Budget: " SIZE_FORMAT
                     ", Claimed Alloc: " SIZE_FORMAT,
                     _original_budget, _original_debt, _original_work, _work_credit, _debt_payment, _new_debt,
                     _budget_increment, _new_budget, bytes);
#endif
  return true;
}

void ShenandoahPacer::unpace_for_alloc(intptr_t epoch, size_t words) {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");
  assert(!_is_generational, "Not expecting this in generational pacing");

#ifdef KELVIN_VERBOSE
  log_info(gc, ergo)("unpace_for_alloc(epoch: " SIZE_FORMAT ", words: " SIZE_FORMAT, epoch, words);
#endif

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
#ifdef KELVIN_VERBOSE
  _pacer_total_alloc += words * HeapWordSize;
  log_info(gc, ergo)("pace_for_alloc(bytes: " SIZE_FORMAT " of " SIZE_FORMAT "), epoch " SIZE_FORMAT,
                     words * HeapWordSize, _pacer_total_alloc, epoch());
#endif
  if (_is_generational) {
    double start = 0.0;
    intptr_t pacer_epoch;
    if (!JavaThread::current()->is_attaching_via_jni() && is_generational_claim_at_risk(words)) {
      // Insert one stall in an effort to distribute stalls across multiple threads and time spans.
      start = os::elapsedTime();
      pacer_epoch = epoch();
      wait(ShenandoahPacingMaxDelay);
    }

    // With generational GC, this service never forces the claim.  We wait until GC has caught up with allocators.
    bool claimed = claim_for_generational_alloc(words);
    if (claimed) {
      if (start != 0.0) {
        double end = os::elapsedTime();
        double delay = end - start;
        ShenandoahThreadLocalData::add_paced_time(JavaThread::current(), delay);
#ifdef KELVIN_VERBOSE
        log_info(gc, ergo)("  pace_for_alloc(bytes: " SIZE_FORMAT ") successfully claimed after delay of %0.3f",
                           words * HeapWordSize, delay);
      } else {
        log_info(gc, ergo)("  pace_for_alloc(bytes: " SIZE_FORMAT ") successfully claimed, returning",
                           words * HeapWordSize);
#endif
      }
      return;
    }

    // Threads that are attaching should not block at all: they are not fully initialized yet. Blocking them would be
    // awkward.  This is probably the path that allocates the thread oop itself.
    if (JavaThread::current()->is_attaching_via_jni()) {
      force_generational_claim_for_alloc(words);
#ifdef KELVIN_VERBOSE
      log_info(gc, ergo)("  pace_for_alloc(bytes: " SIZE_FORMAT ") not stalling because is_attaching_via_jni",
                         words * HeapWordSize);
#endif
      return;
    }
    if (start != 0.0) {
      double start = os::elapsedTime();
      intptr_t pacer_epoch = epoch();
    }
    do {
      // We could instead assist GC, but this would suffice for now.
      wait(ShenandoahPacingMaxDelay);
      bool claimed = claim_for_generational_alloc(words);
      if (claimed) {
        // Exit if the budget has been replenished, which means our claim is satisfied.
        double end = os::elapsedTime();
        double delay = end - start;
        ShenandoahThreadLocalData::add_paced_time(JavaThread::current(), delay);
#ifdef KELVIN_VERBOSE
        log_info(gc, ergo)("  pace_for_alloc(bytes: " SIZE_FORMAT ") successfully claimed after delay of %0.3f",
                           words * HeapWordSize, delay);
#endif
        return;
      }
    } while (epoch() == pacer_epoch);

    // Exit since the pacer epoch has advanced.  It may be that pacing is disabled in the new epoch, but in case
    // pacing is still enabled, account for the allocation here.
    force_generational_claim_for_alloc(words);
    double end = os::elapsedTime();
    double delay = end - start;
    ShenandoahThreadLocalData::add_paced_time(JavaThread::current(), delay);
#ifdef KELVIN_VERBOSE
    log_info(gc, ergo)("  pace_for_alloc(bytes: " SIZE_FORMAT ") successfully claimed at change of epoch after delay of %0.3f",
                       words * HeapWordSize, delay);
#endif
    return;

  } else {
    // Fast path: try to allocate right away

    bool claimed = claim_for_alloc(words, false);
    if (claimed) {
#ifdef KELVIN_VERBOSE
      log_info(gc, ergo)("  pace_for_alloc() successfully claimed, returning");
#endif
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
    if (JavaThread::current()->is_attaching_via_jni()) {
#ifdef KELVIN_VERBOSE
      log_info(gc, ergo)("  pace_for_alloc() not stalling because is_attaching_via_jni");
#endif
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

      intptr_t budget = Atomic::load(&_budget);
      if (total_ms > max_ms || budget >= 0) {
#ifdef KELVIN_VERBOSE
        log_info(gc, ergo)("  pace_for_alloc() done with stall, total_ms: " SIZE_FORMAT
                           ", max_ms: " SIZE_FORMAT ", _budget: " SIZE_FORMAT, total_ms, max_ms, budget);
#endif
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
