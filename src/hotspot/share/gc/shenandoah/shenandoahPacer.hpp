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

#include "gc/shenandoah/shenandoahNumberSeq.hpp"
#include "gc/shenandoah/shenandoahPadding.hpp"
#include "gc/shenandoah/shenandoahSharedVariables.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
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
  volatile intptr_t _epoch;
  volatile double _tax_rate;

  // Heavily updated, protect from accidental false sharing
  shenandoah_padding(0);
  volatile intptr_t _budget;
  shenandoah_padding(1);

  // Heavily updated, protect from accidental false sharing
  shenandoah_padding(2);
  volatile intptr_t _progress;
  shenandoah_padding(3);

public:
  ShenandoahPacer(ShenandoahHeap* heap) :
          _heap(heap),
          _last_time(os::elapsedTime()),
          _progress_history(new TruncatedSeq(5)),
          _wait_monitor(new Monitor(Mutex::safepoint-1, "ShenandoahWaitMonitor_lock", true)),
          _epoch(0),
          _tax_rate(1),
          _budget(0),
          _progress(PACING_PROGRESS_UNINIT) {}

  void setup_for_idle();
  void setup_for_mark();
  void setup_for_evac();
  void setup_for_updaterefs();

  void setup_for_reset();

  inline void report_mark(size_t words);
  inline void report_evac(size_t words);
  inline void report_updaterefs(size_t words);

  inline void report_alloc(size_t words);

  bool claim_for_alloc(size_t words, bool force);
  void pace_for_alloc(size_t words);
  void unpace_for_alloc(intptr_t epoch, size_t words);

  void notify_waiters();

  intptr_t epoch();

  void flush_stats_to_cycle();
  void print_cycle_on(outputStream* out);

private:
  inline void report_internal(size_t words);
  inline void report_progress_internal(size_t words);

  inline void add_budget(size_t words);
  void restart_with(size_t non_taxable_bytes, double tax_rate);

  size_t update_and_get_progress_history();

  void wait(size_t time_ms);
};

/**
 * ShenandoahThrottler provides a mechanism to delay allocations if GC is under duress.  The objective is similar to
 * ShenandoahPacer but the implementation is different.
 *
 * Throttling does not endeavor to tax all threads equally, nor does it endeavor to distribute taxation uniformly throughout
 * the GC cycle.  Rather, throttling operates under the assumption that taxation is usually not required because GC is usually
 * going to complete before we deplete the mutator allocation pool.  Thus, throttling biases taxation toward the end of each
 * GC phase, and it only incurs taxes if the risk is high that mutator allocation pool will be exhausted before we finish GC.
 * The goal is to avoid all taxation for nearly all GC cycles.
 *
 * Note that throttling is not designed to fix improper provisioning of memory and CPU resources for a particular workload.
 * Throttling assumes that resource provisioning is sufficient for the workload.  Throttling is designed to allow robust
 * recovery from a late GC trigger.  The ability to recover robustly from late GC triggers allows us to trigger less
 * conservatively, which ultimately allows more efficient operation because there are less frequent GC cycles.
 *
 * The general approach is as follows:
 *
 * 1. At start of GC, we consult mutator free.  We treat this memory as our budget for completion of GC.
 * 2. We set aside 3/4 of this budget for concurrent marking, and assume that ALL of used is going to be live.  The normal
 *    reality is that only a small fraction (e.g. 5%) of used memory is live so we normally complete marking well before we
 *    have exhausted this budget.
 * 3. At end of marking, we reassess our budgets:
 *    a. Hopefully, we have completed marking without expending our entire budget.  Whatever budget was unspent is now
 *       rebudgeted for the use of evac and update-ref.
 *    b. We may have been able to reclaim immediate garbage.  If so, add this to the budget for evac and update-ref.
 *    c. After selecting the collection set, compute estimates of the work to be performed during evac and update-refs:
 *         i. evac work is the sum of live in the evacuated regions plus 1/16 the sum of live in regions to be promoted
 *            in place. 
 *        ii. update-refs work is one fourth the sum of live in non-collected young regions plus (1/32 of live in old
 *            regions if this is normal evac, or one fourth the live data in uncollected old regions if this is a mixed evac)
 *    d. Add the evac and update-refs work estimates, divide byremaining mutator free to determine the ratio between
 *       required work and allowed allocations.
 * 4. Use this ratio to determine the allocation budgets for evac and update-refs.  Then begin the evac effort.
 * 5. At the end of evac, any unspent evac budget is added to update-refs budget.  Begin update-refs.
 * 6. Ratios above are guesstimates of what will work best.  Improvements based on measurements of the running workload might
 *    be implemented in the TODO future.  For example, measure thecurrent workload to see how the cost of update-refs compares
 *    to the cost of evac per word of live memory (probably does not equal 4:1).  And measure the cost of c&f on promote-in-place
 *    regions to see how this compares to the cost of evacuation (probably does not equal 16:1).  And measure the cost of
 *    update-refs on remembered set vs update-refs without remembered set (probably does not equal 32:1).
 * 7. There are three major phases of GC. They are mark, evac, udpate-refs.  Throttling depends on the phase:
 *    a. During mark (most conservative budget and most unpredictable work): 
 *         i. At start of mark, grant 1/2 of the total mark budget
 *        ii. After 1/8 of predicted work is completed, grant additional 1/4 of mark budget
 *       iii. After additional 1/8 of predicted work is completed, grant 1/8 of mark budget
 *        iv. After additional 1/4 of predicted work is completed (so total of 1/2 of predicted work is completed), grant
 *            remaining 1/8 of mark budget.
 *         v. This leaves 1/2 of the predicted work to be completed in only 1/8 of budget, but that's normally ok, because
 *            the predicted work was extremely conservative.  Remember, that our goal is to never throttle any allocations.
 *    b. During evac and update-refs, we have much better understandingf how much work must be completed.  Still, the progress
 *       monitoring mechanisms is imprecise and the rate of progress may not be "constant".  In order to reduce the likelihood
 *       that we will throttle unnecessarily:
 *         i. At start of phase, grant 1/2 of the total phase budget
 *        ii. After 1/4 of predicted work is completed, grant additional 1/4 of phase budget
 *       iii. After additional 1/4 of predicted work is completed, grant additional 1/8 of phase budget
 *        iv. After additional 1/4 of work is completed (so total of 3/4 of predicted work has been completed), grant the
 *            remainng 1/8 of phase budget.
 *         v. This leaves 1/4 of total predicted work to be completed in only 1/8 of budget.  Normally, this will be ok,
 *            because under normal operation, no throttling is required.
 * 8. How does Throttling affect mutator threads?
 *    a. During times in which young GC is idle, there is not impact on mutator allocation requests.  (A future TODO enhancement
 *       might endeavor to assure a certain minimum percent of CPU is available for concurrent old-gen marking.)
 *    b. Mutator threads allocate from within their TLABS without any impact of throttling.  Throttling only intervenes when a
 *       mutator thread attempts to expand its TLAB bubber.
 *    b. Before we acquire the lock to perform an allocation, we ask if there is enough budget to authorize the requested
 *       allocation.  If so, proceed normally, If not:
 *         i. If this is a TLAB request and ShenandoahElasticTLAB, try resizing the request by MAX(size/8, min-tlab-size) and
 *            retrying the authorization request.
 *        ii. If that doesn't work, sleep for 2 ms and retry the (smaller-sized) authorization request.  (The GC worker
 *            threads will increase the authorization budget as they complete increments of GC work.)
 *       iii. If that doesn't work, double my sleep time (up to a maximum sleep time of 16 ms) and retry the authorization
 *            request. 
 *        iv. Repeat step iii until successful or until this throttle epoch has terminated.  The throttle epoch indicates the
 *            current GC cycle.  If authorization is still declined even after this GC cycle has ended, then we missed the
 *            idle span between GCs because we are in back-to-back GCs.
 *         v. Be sure to handle safepoint requests while we're inside our throttle loop.  There may still be a reason to
 *            degenerate or upgrade to Full GC, as motivated by a GC worker thread or by some other mutator thread.
 *    c. Note that this mechanism will tend to throttle heavy allocators more than light allocators.  For example, if one
 *       mutator thread attempts to allocate a large shared object (larger than TLAB sizes) that thread will be likely to
 *       block until the end of the GC cycle.  If one thread allocates multiple TLABs within a single GC phase, that thread
 *       is likely to experience more stalls than threads that do not need to replenish their TLABs.
 *
 * Comment on value of Throttling: Why not just degenerate?  
 *
 *  1. Degenerate requires all mutator and collector threads to reach safepoint.  This results in "lost" progress.
 *     Cancelling GC sometimes results in 10-100 ms cancellation times.
 *  2. Degeneration forces all threads to pause even though only a limited subset of the total thread population may not
 *     be able to make progress.
 *  3. When we degenerate, all of the TLAB memory that has been distributed among tens or hundreds of running mutator
 *     threads is wasted.  The throttling mechanism allows mutator threads that have available TLAB memory to continue running.
 *  4. Degeneration is generally less efficient GC than concurrent GC, because degeneration requires all cores (e.g. 8-32) to
 *     focus on the same job, which increases lock contention and memory bus contention.
 *
 * The implementation of ShenandoahThrottler is completely independent of ShenandoahPacer.  Enabling of ShenandoahThrottler
 * is mutually exclusive with enabling of ShenandoahPacer.  The goal is to completely isolate the code so as to not introduce
 * unintended regressions in the performance of ShenandoahPacer.  We should be able to offer the option of using the
 * the ShenandoahThrottler instead of ShenandoahPacer to users of single-generation Shenandoah.
 */
class ShenandoahThrottler : public CHeapObj<mtGC> {
public:
  // Assume evacuation requires four times the work of updating: Evacuation reads and writes every word.  Updateing only
  // reads and overwrites reference words that refer to the collection set after looking up the forewarding pointer.
  static const uintx EVACUATE_VS_UPDATE_FACTOR;

  // Assume evacuation requires sixteen times the work of promoting in place.  Promote in place only looks at headers of
  // marked objects, and writes new headers between each run of consecutive marked objects.
  static const uintx PROMOTE_IN_PLACE_FACTOR;

  // Assume evacuation requires thirty two times the work of updating references within the remembered set.  Updating
  // within the remembered set only has to deal with pointers that are within DIRTY ranges of the remembered set.
  // Much of the remembered set is not DIRTY and is not pointers.
  static const uintx REMEMBERED_SET_UPDATE_FACTOR;

  // Maximum time to wait in a single throttle delay.  The throttling for any particular allocation request may
  // consist of multiple delays.  Increasing this value would result in a slower response to the eventual availability
  // of memory.  Decreasing this value would result in more operating system toil as a thread repeatedly sleeps and
  // wakes.  Note that a thread waiting in a throttle delay will be notified when additional memory becomes available.
  static const uintx MAX_THROTTLE_DELAY_MS;

  // Minimum time to wait in a single throttle delay.   When a thread finds that it cannot claim authorization for
  // a requested allocation, it first delays this amount.  If, after waiting, it still cannot claim authorization for
  // the allocation, it waits twice this amount, and so on, until the delay equals MAX_THROTTLE_DELAY_MS.  Thereafter,
  // it repeatedly waits MAX_THROTTLE_DELAY_MS until the allocation request can be granted.
  static const uintx MIN_THROTTLE_DELAY_MS;

  enum GCPhase {
    _idle,                      // Waiting to start next GC cycle
    _reset,                     // Reset state to begin new GC cycle
    _mark,                      // Marking live memory
    _evac,                      // Evacuating collection set
    _update,                    // Updating references
    _GCPhase_Count,             // Number of GC Phases
    _not_a_label                // Sentinel value
  };

  enum MicroGCPhase {           // Divide each GC Phase into three micro-cycles
    _first_microphase,
    _second_microphase,
    _third_microphase,
    _Microphase_Count
  };

private:
  ShenandoahHeap* _heap;
  bool _is_generational;
  Monitor* _wait_monitor;
  ShenandoahSharedFlag _need_notify_waiters;

  // Metrics gathered for logging
  size_t _log_effort[_GCPhase_Count];
  size_t _log_progress[_GCPhase_Count];
  size_t _log_budget[_GCPhase_Count];
  size_t _log_authorized[_GCPhase_Count];
  size_t _log_allocated[_GCPhase_Count];

  size_t _log_requests[_GCPhase_Count];
  size_t _log_words_throttled[_GCPhase_Count];
  size_t _log_min_words_throttled[_GCPhase_Count];
  size_t _log_max_words_throttled[_GCPhase_Count];
  double _log_max_time_throttled[_GCPhase_Count];
  double _log_total_time_throttled[_GCPhase_Count];

  size_t _log_failed[_GCPhase_Count];
  size_t _log_words_failed[_GCPhase_Count];
  size_t _log_min_words_failed[_GCPhase_Count];
  size_t _log_max_words_failed[_GCPhase_Count];
  double _log_max_time_failed[_GCPhase_Count];
  double _log_total_time_failed[_GCPhase_Count];

  // set and read once per phase, by control thread.
  GCPhase _phase_label;
  size_t _phase_work;
  size_t _phase_budget;
  size_t _most_recent_live_young_words;
  size_t _most_recent_live_global_words;

  // Set once per phase
  // _work_completed and _budget_supplement are both defined in terms of words of memory.
  size_t _work_completed[_GCPhase_Count];
  size_t _budget_supplement[_GCPhase_Count];

  // _epoch represents the current phase of GC.  This is different from the current GC cycle.
  volatile intptr_t _epoch;

  // Set 4 times per phase
  volatile size_t _phase_authorized;

#ifdef KELVIN_DEPRECATE
  // Set 4 times per phase (as quantums of work are completed)
  volatile size_t _authorized_allocations;
#endif
#undef KELVIN_THROTTLES
#ifdef KELVIN_THROTTLES
  volatile size_t _threads_in_throttle;
#endif
  // Updated rarely, only after an allocation request has been successfully or unsuccessfully throttled
  volatile size_t _allocation_requests_throttled;
  volatile size_t _total_words_throttled;
  volatile size_t _min_words_throttled;
  volatile size_t _max_words_throttled;
  volatile double _max_time_throttled;
  volatile double _total_time_throttled;

  volatile size_t _allocation_requests_failed;
  volatile size_t _total_words_failed;
  volatile size_t _min_words_failed;
  volatile size_t _max_words_failed;
  volatile double _max_time_failed;
  volatile double _total_time_failed;

  // Heavily updated, protect from accidental false sharing
  shenandoah_padding(0);
#ifdef KELVIN_DEPRECATE
  // Words of memory allocated.
  volatile size_t _allocated;
#endif
  volatile intptr_t _available_words;
  shenandoah_padding(1);                // TODO: Is this redundant with shenandoah_padding(2), wasteful? no-op?

  // Heavily updated, protect from accidental false sharing
  shenandoah_padding(2);
  // Words of memory "processed" in the current phase.  Processing is defined differently depending on the phase.
  volatile size_t _progress;
  shenandoah_padding(3);

public:
  ShenandoahThrottler(ShenandoahHeap* heap);

  bool cycle_had_throttles() const;

  void setup_for_mark(size_t allocatable_words, bool is_global);
  void setup_for_evac(size_t allocatable_words, size_t evac_words, size_t promo_in_place_words, size_t uncollected_young_words,
                      size_t uncollected_old_words, bool is_mixed, bool is_global, bool is_bootstrap);
  void setup_for_updaterefs(size_t allocatable_words, size_t promo_in_place_words,
                            size_t uncollected_young_words, size_t uncollected_old_words, bool is_mixed_or_global);

  // Not sure if I want to treat idle, reset as separate phases
  void setup_for_idle(size_t allocatable_words);
  void setup_for_reset(size_t allocatable_words);

  inline void report_mark(size_t words);
  inline void report_evac(size_t words);
  inline void report_updaterefs(size_t words);

  inline void report_alloc(size_t words);

  bool claim_for_alloc(intptr_t words, bool force, bool allow_greed = false);
  size_t throttle_for_alloc(ShenandoahAllocRequest req);
  void unthrottle_for_alloc(intptr_t epoch, size_t words);

  void notify_waiters();

  intptr_t epoch();

  void flush_stats_to_cycle();
  void print_cycle_on(outputStream* out);

private:
  void publish_metrics();
  void reset_metrics(GCPhase id, size_t planned_work, size_t budget, size_t initial_authorization);
  void add_to_metrics(bool successful, size_t words, double delay);
  void log_metrics_and_prep_for_next();

  inline void report_internal(size_t words);
  inline void report_progress_internal(size_t words);

  inline void wake_throttled();

  inline void add_budget(size_t words);

#ifdef KELVIN_DEPRECATE
  void restart_with(size_t non_taxable_bytes, double tax_rate);
#endif

  const char* phase_name(GCPhase p);

  void wait(size_t time_ms);
};


#endif // SHARE_GC_SHENANDOAH_SHENANDOAHPACER_HPP
