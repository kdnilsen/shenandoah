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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHPACER_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHPACER_HPP

#ifdef KELVIN_FUBAR
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#endif
#include "gc/shenandoah/shenandoahNumberSeq.hpp"
#include "gc/shenandoah/shenandoahPadding.hpp"
#include "memory/allocation.hpp"

class ShenandoahHeap;

#define PACING_PROGRESS_UNINIT (-1)
#define PACING_PROGRESS_ZERO   ( 0)

/**
 * ShenandoahPacer provides allocation pacing mechanism.
 *
 * Currently it implements simple tax-and-spend pacing policy: GC threads provide
 * credit, allocating thread spend the credit, or stall when credit is not available.
 */
class ShenandoahPacer : public CHeapObj<mtGC> {
private:
  ShenandoahHeap* _heap;
  double _last_time;
  TruncatedSeq* _progress_history;
  Monitor* _wait_monitor;
  ShenandoahSharedFlag _need_notify_waiters;

  // Set once per phase
  intptr_t _planned_old_marking_passes;               // Generational: how many old-gen increments of work planned for marking
  intptr_t _planned_old_mixed_evac_prep_passes;       // Generational: how many old-gen increments of work planned for evac prep
  size_t _total_planned_old_effort;                   // Generational: Total effort planned for current old effort, replenished
                                                      //               at start of concurrent mark and evac-prep passes
  size_t _last_mark_increment_supplement;             // Generational: plan to invest this much extra effort in the last increment
                                                      //               of mark work if original _total_planned_old_effort was
                                                      //               not sufficient to complete marking effort
  size_t _concurrent_old_interruption_count;          // Generational: how many times has old-gen been interrupted
  size_t _most_recent_young_live_bytes;               // Generational: young-gen bytes live at end of most recent young mark (0
                                                      //               means not yet initialized)
  size_t _most_recent_old_live_bytes;                 // Generational: old-gen bytes live at end of most recent old mark (0
                                                      //               means not yet initialized)
  size_t _usage_below_update_watermark;               // Generational: how much memory to be processed by update refs

  volatile size_t _allocation_per_work_ratio_per_K;   // Generational: 1024 times the number of heapwords that can be
                                                      // allocated for each heapword of work completed.

  volatile intptr_t _epoch;
  volatile double _tax_rate;

  // Generational Shenandoah uses different experimental pacing mechanisms than traditional Shenandoah.
  // If these new pacing mechanisms prove to be more effective at reducing degenerated and full GC triggers
  // and minimizing the impact of allocation stalls, we should explore the possibility of applying these
  // same aproaches to traditional Shenandoah.  Here's the "theory of operation":
  //
  //  1. At the start of each major concurrent GC phase (marking, evacuation, updating references), we estimate
  //     how much total work needs to be completed and we establish a budget for how much total memory we will allow
  //     ourselves to allocate while we are performing the efforts associated with this phase effort.
  //
  //  2. We establish an allocation_budget_ratio that controls the rate at which the limit on allowed allocations is
  //     increased in proportion to completion of work efforts.
  //
  //  3. Each time an allocation is requested, we check whether sufficient work has been completed to justify the
  //     allocation.  If not, we stall the allocating thread, possibly multiple times until we are "on pace".
  //
  //  4. Whenever pacing is enabled, there will not be an allocation failure during young collection.  Instead, there
  //     will be allocation stalls.  TODO: after an excessive number of allocation stalls, we may choose to report allocation
  //     failure or throw OutOfMemoryException.  How do we quantify "excessive number"?
  //
  //  5. Note that generational Shenandoah pacing is not enabled while young-gen GC is idle except in the very rare case
  //     that we have underestimated the total effort required to complete a particular GC phase.  During these times, it
  //     is the responsibility of ShenandoahHeuristics to trigger young-gen GC with sufficient advance notice to avoid an
  //     allocation failure.  Reduce the risk of a heuristics triggering failure by setting the ShenandoahAllocSpikeFactor
  //     to a higher value.  TODO: we may want to prevent allocation failures from "slipping between the cracks" by checking
  //     for young_heuristics::should_start_gc() at the very end of each young-gen cycle.
  //
  // Tuning refinements:
  //
  //  1. To avoid unnecessary stalls "immediately" after we begin the work for a particular phase, we preauthorize
  //     a limited amount of allocations to be performed before the relevant work has been completed.  Up to 25% of
  //     the allocation credit earned with completion of any new work is spent "paying off" the preauthorization debt.
  //
  //  Efficiency considerations:
  //
  //  1. Increments of work are typically reported more frequently than allocations (because we use local allocation
  //     buffers to reduce synchronization).
  //
  //  2. We tend to experience bursts of allocation requests at the starts of certain concurrent phases of GC.  This is
  //     because some transitions between GC phases are required to discard and replace all local allocation buffers.
  //
  //  3. TODO: We might reduce need for atomic reads by buffering within thread-local variables the value of
  //     _work_allocation_ratio, which only changes at the start of each GC phase.
  //
  //  4. TODO: Consider whether we want to invest in "fairness".  It could be that one thread is penalized many times
  //     while other threads are not being penalized.  This can happen on a single allocation request that is penalized
  //     repeatedly, or it can happen if the same thread issues multiple allocation requests in rapid succession and
  //     each of these is penalized.  My initial thought is that we don't try to interfere with the existing pacing
  //     implementation as (a) pacing will only introduce stalls if the system is "under provisioned" (at least
  //     temporarily) and (b) if this does happen, it is most likely that the thread receiving multiple penalties is
  //     a thread that is aggressively allocating.  Note that the behavior of generational shenandoah differs from
  //     traditional shenandoah in this regard.  Traditional Shenandoah will only stall any particular allocation
  //     request once and then it will "force" the allocation to proceed, possibly placing the allocation budget into
  //     arrears.  That approach has the undesirable consequence that a single humongous allocation request that is
  //     granted following a short allocation stall will leave the allocation budget in such a shortfall that all other
  //     less greedy threads will be forced to stall in their allocation requests that follow.  In most cases, it
  //     would be expected that threads (and activities) that perform aggresive allocations would have less predictable
  //     and generally longer response time targets than threads that have "reasonable" or light allocation rates.
  //
  //  5. TODO: Consider whether we want to pack the four volatile variables into two HeapWords to reduce burden of
  //     Atomic fetches and overwrites.  This could be certainly be done when we are running Compressed OOPs.  Potentially,
  //     this could also be achieved even when not running Compressed OOPs by "rounding down" approximations of work
  //     completed and budget remaining:
  //     a. Pack _incremental_phase_work_completed and _incremental_allocation_budget into one HeapWord, with 32-bits
  //        dedicated to each value, and the units of each representing HeapSize / 2^32.  Never increment phase work completed
  //        to equal 0xffffffff because this token value represents the special condition that pacing is disabled.  In practice,
  //        we would never expect phase work completed to match this value.  That would require essentiallly 100% of the
  //        total heap to be live (during marking and evacuation or update refs).  Suppose, for example, we have a 64 GB
  //        heap.  Then each work unit and each allocation budget unit represents 2 HeapWords.  For conservative operation,
  //        we could round work completed down and allocation requests up.  Or we could use a non-linear representation of
  //        both quantities.  For values less than or equal to 0x80000000, the value represents the exact number of heap
  //        words. For values greater than 0x8000000, the value X represents 0x80000000 + (X - 0x80000000) * 4 heapwords.
  //        The fast path through the shenandoahPacing implementation only needs to atomically load this single heap word.
  //     b. Pack _preauthorization_debt and _allocation_per_work_ratio_per_K into a second HeapWord.  Since _preauthorization_debt
  //        may be no larger than 1/4 of young-gen available, compression of its representation may not be necessary.  Similar
  //        arguments may obviate the need to compress _allocation_per_work_ratio_per_K.
  //
  //  Implementation Details:
  //
  //  1. At the start of a concurrent young-gen GC phase, we initialize the following shared "atomic" variables:
  //       _incremental_phase_work_completed = 0
  //       _incremental_allocation_budget = <preauthorization_amount>
  //       _preauthorization_debt = <preauthorization_amount>;
  //       _allocation_per_work_ratio_per_K = <allocation_budget_for_phase> * 1024 / <anticipated_work_for_phase>
  //
  //     Notes: The preauthorization_budget may be larger for phases that typically need to perform multiple allocations
  //            immediately following the start of the phase (e.g. concurrent marking needs to replenish all local allocation
  //            buffers to honor TAMS and concurrent evacuation needs to replenish local allocation buffers to honor
  //            update_watermark).  The preauthorization budget shall never exceed 1/4 the <allocation_budget_for_phase>.
  //
  //  2. When young-gen GC becomes idle, we disable pacing by setting the following variable:
  //       _incremental_phase_work_completed = -1;
  //
  //  3. Whenever an increment of work is reported, we atomically add the accounting to _incremental_phase_work_completed
  //
  //  4. When allocation of size requested_allocation_size is requested:
  //     a. Atomically fetch _incremental_phase_work_completed into local variable phase_work_completed
  //     b. If (phase_work_completed < 0), then pacing is disabled and we ignore the remaining steps described below
  //     c  . Atomically fetch _incremental_allocation_budget into local allocation_budget
  //     d.   If (phase_work_completed == 0), ignore steps e through kbelow
  //     e.     Atomically fetch _allocation_per_work_ratio_per_K into local variable work_ratio
  //     f.     Atomically fetch _preauthorization_debt into local variable debt.
  //     g.     Calculate allocation_budget_adjustment = (phase_work_completed * work_ratio) / 1024;
  //     h.     Calculate debt_payment = Min(debt, allocation_budget_adjustment / 4)
  //     i.     Atomically decrement _preauthorization_debt by debt_payment
  //     j.     Adjust allocation_budget_adjustment -= debt_payment
  //     k.     If (allocation_budget_adjustment > 0) then increment allocation_budget by allocation_budget_adjustment
  //     l.   if (allocation_budget < requested_allocation_size)
  //     m.     Atomically write allocation_budget to _incremental_allocation_budget (so some other thread can use the budget)
  //     n.     Stall this thread
  //     o.     Proceed to step (a)
  //     p.   Attempt to claim the allocation by atomically decrementing _incremental_allocation_budget by
  //            requested_allocation_size.
  //     q.     I can retry the atomic decrement operation multiple times, adjusting allocation_budget by any changes to
  //            _incremental_allocation_budget, and retrying the attempt to claim the memory allocation as long
  //            as allocation_budget > requested_allocation_size.  But if _incremental_allocation_budget ever drops below 
  //            requested_allocation_size, proceed to step m.
  //
  // Discussion of "pacing overhead":
  //    Best case (typical case under light workload): we do one atomic fetch for each allocation
  //    If allocation requests are more frequent than work updates (which we assert is not the typical case), overhead is:
  //      Two atomic fetches (_incremental_phase_work_completed and _incremental_allocation_budget) and one atomic
  //      store (_incremental_allocation_budget)
  //    If allocation requests are less frequent than work updates and stalls are not necessary, overhead is:
  //      Four atomic fetches (_incremental_phase_work_completed, _incremental_allocation_budget, and
  //      _allocation_per_work_ratio_per_K, _preauthorization_debt) and three atomic stores (_incremental_phase_work_completed,
  //      _incremental_allocation_budget, and _preauthorization_debt).
  //    When pacing is required or access to overwritten atomic variables is contended, additional overheads are
  //      expected, but this should be rare.

  // Heavily updated, protect from accidental false sharing

  static const size_t PHASE_WORK_PACING_DISABLED = (size_t) -1;


  // TODO: each shenandoah_padding() instance introduces a 64-byte character buffer.  This intends to assure
  // that each heavily updated field resides within a distinct cache line.  We could achieve the same benefits
  // more concisely by inserting padding of size 64-8 = 56 bytes between each heavily updated HeapWord field.
  shenandoah_padding(0);
  volatile intptr_t _budget;  // non-generational: similar to _incremental_allocation_budget.  could union with that.
  shenandoah_padding(1);
  // Allow these three variables to occupy the same cache line because we'll generally want their values together.
  volatile size_t _incremental_phase_work_completed; // for generational, increments of work that have been completed
  volatile size_t _incremental_allocation_budget;      // for generational, how much memory can be allocated right now?
  volatile size_t _preauthorization_debt;              // for generational, debt to be paid on allocation preauthorizations
#define KELVIN_FIX_COMPILE
#ifdef KELVIN_FIX_COMPILE
  // One incremental commit along the way to ShenandoahPacing makes mention of these variables without defining them
  volatile size_t _anticipated_phase_effort;
  volatile size_t _allocation_phase_budget;
  volatile size_t _previous_old_live;
  volatile size_t _planned_old_mixed_evacuation_passes;
  volatile size_t _last_marking_increment_supplemental_effort;
  volatile size_t _planned_prep_mixed_evacuation_passes;
  volatile size_t _total_planned_effort;
#endif

  // Heavily updated, protect from accidental false sharing
  shenandoah_padding(2);
  volatile intptr_t _progress;  // non-generational: similar to _incremental_phase_work_completed.  could union with that.
  shenandoah_padding(3);

#ifdef KELVIN_FUBAR
  // Put this bool field after the HeapWord-sized fields for improved alignment and padding.
  const bool _is_generational;
#endif
public:
  ShenandoahPacer(ShenandoahHeap* heap) :
          _heap(heap),
          _last_time(os::elapsedTime()),
          _progress_history(new TruncatedSeq(5)),
          _wait_monitor(new Monitor(Mutex::safepoint-1, "ShenandoahWaitMonitor_lock", true)),
          _concurrent_old_interruption_count(0),
          _most_recent_young_live_bytes(0),
          _most_recent_old_live_bytes(0),
          _usage_below_update_watermark(0),
          _allocation_per_work_ratio_per_K(0),
          _epoch(0),
          _tax_rate(1),
          _budget(0),
          _incremental_phase_work_completed(0),
          _incremental_allocation_budget(0),
          _preauthorization_debt(0),
          _progress(PACING_PROGRESS_UNINIT) {
#ifdef KELVIN_FUBAR
    _is_generational = _heap->mode()->is_generational();
#endif
  }

  void setup_for_idle();
  void setup_for_mark();
  void setup_for_evac();
  void setup_for_updaterefs();

  void setup_for_reset();

  inline void report_mark(size_t words);
  inline void report_evac(size_t words);
  inline void report_updaterefs(size_t words);

  inline void report_alloc(size_t words);

  bool claim_for_generational_alloc(size_t words);
  bool claim_for_alloc(size_t words, bool force);
  void pace_for_alloc(size_t words);
  void unpace_for_alloc(intptr_t epoch, size_t words);

  void notify_waiters();

  intptr_t epoch();

  void flush_stats_to_cycle();
  void print_cycle_on(outputStream* out);

private:
#ifndef KELVIN_FUBAR
  bool is_generational();
#endif
  inline void report_internal(size_t words);
  inline void report_progress_internal(size_t words);

  inline void add_budget(size_t words);
  void restart_with(size_t non_taxable_bytes, double tax_rate);

  size_t update_and_get_progress_history();

  void wait(size_t time_ms);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHPACER_HPP
