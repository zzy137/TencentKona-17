/*
 * Copyright (c) 2015, 2020, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1IHOPControl.hpp"
#include "gc/g1/g1Predictions.hpp"
#include "gc/g1/g1Trace.hpp"
#include "logging/log.hpp"

G1IHOPControl::G1IHOPControl(double initial_ihop_percent,
                             G1OldGenAllocationTracker const* old_gen_alloc_tracker) :
  _initial_ihop_percent(initial_ihop_percent),
  _target_occupancy(0),
  _last_allocation_time_s(0.0),
  _old_gen_alloc_tracker(old_gen_alloc_tracker)
{
  assert(_initial_ihop_percent >= 0.0 && _initial_ihop_percent <= 100.0, "Initial IHOP value must be between 0 and 100 but is %.3f", initial_ihop_percent);
}

void G1IHOPControl::update_target_occupancy(size_t new_target_occupancy) {
  log_debug(gc, ihop)("Target occupancy update: old: " SIZE_FORMAT "B, new: " SIZE_FORMAT "B",
                      _target_occupancy, new_target_occupancy);
  _target_occupancy = new_target_occupancy;
}

void G1IHOPControl::update_allocation_info(double allocation_time_s, size_t additional_buffer_size) {
  assert(allocation_time_s >= 0.0, "Allocation time must be positive but is %.3f", allocation_time_s);

  _last_allocation_time_s = allocation_time_s;
}

void G1IHOPControl::print() {
  assert(_target_occupancy > 0, "Target occupancy still not updated yet.");
  size_t cur_conc_mark_start_threshold = get_conc_mark_start_threshold();
  log_debug(gc, ihop)("Basic information (value update), threshold: " SIZE_FORMAT "B (%1.2f), target occupancy: " SIZE_FORMAT "B, current occupancy: " SIZE_FORMAT "B, "
                      "recent allocation size: " SIZE_FORMAT "B, recent allocation duration: %1.2fms, recent old gen allocation rate: %1.2fB/s, recent marking phase length: %1.2fms",
                      cur_conc_mark_start_threshold,
                      percent_of(cur_conc_mark_start_threshold, _target_occupancy),
                      _target_occupancy,
                      G1CollectedHeap::heap()->used(),
                      _old_gen_alloc_tracker->last_period_old_gen_bytes(),
                      _last_allocation_time_s * 1000.0,
                      _last_allocation_time_s > 0.0 ? _old_gen_alloc_tracker->last_period_old_gen_bytes() / _last_allocation_time_s : 0.0,
                      last_marking_length_s() * 1000.0);
}

void G1IHOPControl::send_trace_event(G1NewTracer* tracer) {
  assert(_target_occupancy > 0, "Target occupancy still not updated yet.");
  tracer->report_basic_ihop_statistics(get_conc_mark_start_threshold(),
                                       _target_occupancy,
                                       G1CollectedHeap::heap()->used(),
                                       _old_gen_alloc_tracker->last_period_old_gen_bytes(),
                                       _last_allocation_time_s,
                                       last_marking_length_s());
}

G1StaticIHOPControl::G1StaticIHOPControl(double ihop_percent,
                                         G1OldGenAllocationTracker const* old_gen_alloc_tracker) :
  G1IHOPControl(ihop_percent, old_gen_alloc_tracker),
  _last_marking_length_s(0.0) {
}

G1AdaptiveIHOPControl::G1AdaptiveIHOPControl(double ihop_percent,
                                             G1OldGenAllocationTracker const* old_gen_alloc_tracker,
                                             G1Predictions const* predictor,
                                             size_t heap_reserve_percent,
                                             size_t heap_waste_percent) :
  G1IHOPControl(ihop_percent, old_gen_alloc_tracker),
  _heap_reserve_percent(heap_reserve_percent),
  _heap_waste_percent(heap_waste_percent),
  _predictor(predictor),
  _marking_times_s(10, 0.05),
  _allocation_rate_s(10, 0.05),
  _last_unrestrained_young_size(0)
{
}

size_t G1AdaptiveIHOPControl::actual_target_threshold() const {
//这个断言确保_target_occupancy已经被更新不为0
  guarantee(_target_occupancy > 0, "Target occupancy still not updated yet.");
  // The actual target threshold takes the heap reserve and the expected waste in
  // free space  into account.
  // _heap_reserve is that part of the total heap capacity that is reserved for
  // eventual promotion failure.
  // _heap_waste is the amount of space will never be reclaimed in any
  // heap, so can not be used for allocation during marking and must always be
  // considered.
//安全的堆占用阈值 这个表达式计算了安全的总堆容量百分比。它是 _heap_reserve_percent 和 _heap_waste_percent 之和,但不能超过 100%。
  double safe_total_heap_percentage = MIN2((double)(_heap_reserve_percent + _heap_waste_percent), 100.0);

//这个返回语句计算并返回实际的目标阈值。
//第一个表达式计算了最大堆容量乘以 (100% - 安全总堆容量百分比)。这给出了可用于分配的最大空间。
//第二个表达式计算了目标占用率乘以 (100% - 预期浪费百分比)。这给出了实际的目标阈值。
//两个表达式取较小的值作为最终的实际目标阈值。
  return (size_t)MIN2(
    G1CollectedHeap::heap()->max_capacity() * (100.0 - safe_total_heap_percentage) / 100.0,
    _target_occupancy * (100.0 - _heap_waste_percent) / 100.0
    );
}
//这个方法用于根据给定的 TruncatedSeq 对象进行预测,并返回一个受下限为 0 的值。
//TruncatedSeq 是一个用于存储和管理有限长度序列的类。在 G1AdaptiveIHOPControl 中,它被用于存储最近的并发标记持续时间和老年代分配速率。
//predict_zero_bounded() 方法可能会使用各种预测算法,如线性回归、时间序列分析等,来根据给定的序列数据进行预测。这个预测值会被用于计算并发标记开始阈值。
double G1AdaptiveIHOPControl::predict(TruncatedSeq const* seq) const {
//_predictor 是一个指向 G1Predictions 对象的指针,它提供了预测功能。
  return _predictor->predict_zero_bounded(seq);
}
//这是 have_enough_data_for_prediction() 方法的签名。它返回一个布尔值,表示是否有足够的数据进行预测。
//这个返回语句检查了两个条件:
 // _marking_times_s 中的样本数是否大于或等于 G1AdaptiveIHOPNumInitialSamples。
 //_allocation_rate_s 中的样本数是否大于或等于 G1AdaptiveIHOPNumInitialSamples。
 //如果两个条件都满足,则返回 true，表示有足够的数据进行预测。否则返回 false。
bool G1AdaptiveIHOPControl::have_enough_data_for_prediction() const {
  return ((size_t)_marking_times_s.num() >= G1AdaptiveIHOPNumInitialSamples) &&
         ((size_t)_allocation_rate_s.num() >= G1AdaptiveIHOPNumInitialSamples);
}

//用于计算并发标记应该开始的堆占用率阈值。（ihop）
size_t G1AdaptiveIHOPControl::get_conc_mark_start_threshold() {
//这个条件检查是否有足够的数据进行预测。如果有足够的数据,则进入 if 块执行动态计算。否则使用初始值。
  if (have_enough_data_for_prediction()) {
  //这行代码调用 predict() 方法,使用 _marking_times_s 序列预测并发标记的持续时间
    double pred_marking_time = predict(&_marking_times_s);
    //这行代码调用 predict() 方法,使用 _allocation_rate_s 序列预测老年代的分配速率。
    double pred_promotion_rate = predict(&_allocation_rate_s);
    //这行代码计算在预测的并发标记持续时间内,预计会分配的老年代字节数。
    size_t pred_promotion_size = (size_t)(pred_marking_time * pred_promotion_rate);

//这行代码计算在并发标记期间需要的总字节数,包括预测的老年代分配和最近的 young 代大小。
    size_t predicted_needed_bytes_during_marking =
      pred_promotion_size +
      // In reality we would need the maximum size of the young gen during
      // marking. This is a conservative estimate.
      _last_unrestrained_young_size;

//这行代码调用 actual_target_threshold() 方法,获取实际的目标阈值。
    size_t internal_threshold = actual_target_threshold();
    //这行代码计算预测的并发标记开始阈值。如果预测的需求字节数小于实际目标阈值,则使用实际目标阈值减去预测需求的结果。否则返回 0。
    size_t predicted_initiating_threshold = predicted_needed_bytes_during_marking < internal_threshold ?
                                            internal_threshold - predicted_needed_bytes_during_marking :
                                            0;
    return predicted_initiating_threshold;
  } else {
    // Use the initial value.
    return (size_t)(_initial_ihop_percent * _target_occupancy / 100.0);
  }
}
//这个方法用于计算上一个 mutator 期间老年代的分配速率。
//这个返回语句计算上一个 mutator 期间老年代的分配速率。
//  _old_gen_alloc_tracker->last_period_old_gen_growth() 获取上一个 mutator 期间老年代的增长量。
//  将这个增长量除以 _last_allocation_time_s(上一个 mutator 期间的持续时间),就得到了老年代的分配速率。
double G1AdaptiveIHOPControl::last_mutator_period_old_allocation_rate() const {
  assert(_last_allocation_time_s > 0, "This should not be called when the last GC is full");
  //_old_gen_alloc_tracker 是一个指向 G1OldGenAllocationTracker 对象的指针,用于跟踪老年代的分配情况。last_period_old_gen_growth() 方法返回上一个 mutator 期间老年代的增长量。
  return _old_gen_alloc_tracker->last_period_old_gen_growth() / _last_allocation_time_s;
 }

//这个方法用于更新与分配相关的信息。
void G1AdaptiveIHOPControl::update_allocation_info(double allocation_time_s,
                                                   size_t additional_buffer_size) {
  G1IHOPControl::update_allocation_info(allocation_time_s, additional_buffer_size);
  _allocation_rate_s.add(last_mutator_period_old_allocation_rate());

  _last_unrestrained_young_size = additional_buffer_size;
}
//法用于更新最近一次并发标记阶段的持续时间。
void G1AdaptiveIHOPControl::update_marking_length(double marking_length_s) {
   assert(marking_length_s >= 0.0, "Marking length must be larger than zero but is %.3f", marking_length_s);
  _marking_times_s.add(marking_length_s);
}

void G1AdaptiveIHOPControl::print() {
  G1IHOPControl::print();
  size_t actual_target = actual_target_threshold();
  log_debug(gc, ihop)("Adaptive IHOP information (value update), threshold: " SIZE_FORMAT "B (%1.2f), internal target occupancy: " SIZE_FORMAT "B, "
                      "occupancy: " SIZE_FORMAT "B, additional buffer size: " SIZE_FORMAT "B, predicted old gen allocation rate: %1.2fB/s, "
                      "predicted marking phase length: %1.2fms, prediction active: %s",
                      get_conc_mark_start_threshold(),
                      percent_of(get_conc_mark_start_threshold(), actual_target),
                      actual_target,
                      G1CollectedHeap::heap()->used(),
                      _last_unrestrained_young_size,
                      predict(&_allocation_rate_s),
                      predict(&_marking_times_s) * 1000.0,
                      have_enough_data_for_prediction() ? "true" : "false");

}
//这个方法用于向 G1NewTracer 对象发送与自适应 IHOP 控制相关的统计信息。
void G1AdaptiveIHOPControl::send_trace_event(G1NewTracer* tracer) {
  G1IHOPControl::send_trace_event(tracer);
  //G1NewTracer 是一个用于跟踪 G1 垃圾收集器行为的类。report_adaptive_ihop_statistics() 方法允许 G1AdaptiveIHOPControl 将自适应 IHOP 相关的统计信息发送给跟踪器,以便进行监控和分析。
  tracer->report_adaptive_ihop_statistics(
                                           //并发开始标记阈值
                                          get_conc_mark_start_threshold(),
                                          //实际目标阈值
                                          actual_target_threshold(),
                                          //当前堆占用率
                                          G1CollectedHeap::heap()->used(),
                                          //最近一次未受限制的yong代大小
                                          _last_unrestrained_young_size,
                                          //预测的老年代分配速率
                                          predict(&_allocation_rate_s),
                                          //预测的并发标记的持续时间
                                          predict(&_marking_times_s),
                                          //是否有足够的数据进行预测
                                          have_enough_data_for_prediction());
}
