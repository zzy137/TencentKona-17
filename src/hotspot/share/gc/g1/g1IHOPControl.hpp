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

#ifndef SHARE_GC_G1_G1IHOPCONTROL_HPP
#define SHARE_GC_G1_G1IHOPCONTROL_HPP

#include "gc/g1/g1OldGenAllocationTracker.hpp"
#include "memory/allocation.hpp"
#include "utilities/numberSeq.hpp"

class G1Predictions;
class G1NewTracer;

// Base class for algorithms that calculate the heap occupancy at which
// concurrent marking should start. This heap usage threshold should be relative
// to old gen size.
class G1IHOPControl : public CHeapObj<mtGC> {
 protected:
  // The initial IHOP value relative to the target occupancy.
  //IHPO的初始值
  double _initial_ihop_percent;
  // The target maximum occupancy of the heap. The target occupancy is the number
  // of bytes when marking should be finished and reclaim started.
  //堆的目标最大占用率 目标占用率是标记完成并开始回收时的占用的一个字节数
  size_t _target_occupancy;

  // Most recent complete mutator allocation period in seconds.
  //最近一次完整的 mutator 分配期的持续时间(以秒为单位)。
  double _last_allocation_time_s;

//一个指向 G1OldGenAllocationTracker 对象的指针,用于跟踪老年代的分配情况。
  const G1OldGenAllocationTracker* _old_gen_alloc_tracker;
  // Initialize an instance with the old gen allocation tracker and the
  // initial IHOP value in percent. The target occupancy will be updated
  // at the first heap expansion.
  //一个构造函数，目标占用率会随第一次堆拓展发生变化
  G1IHOPControl(double ihop_percent, G1OldGenAllocationTracker const* old_gen_alloc_tracker);

  // Most recent time from the end of the concurrent start to the start of the first
  // mixed gc.
  //这是一个纯虚函数,子类需要实现它来返回最近一次并发标记阶段的持续时间(以秒为单位)。 从并发启动结束到第一次mixedgc开始的最近时间
  virtual double last_marking_length_s() const = 0;
 public:
 //一个虚析构函数 ，用于确保子类的析构函数能够被正确调用
  virtual ~G1IHOPControl() { }

  // Get the current non-young occupancy at which concurrent marking should start.
  // 这是一个纯虚函数,子类需要实现它来返回并发标记应该开始的非 young 代占用率阈值。
  virtual size_t get_conc_mark_start_threshold() = 0;

  // Adjust target occupancy.
  //
  virtual void update_target_occupancy(size_t new_target_occupancy);
  // Update information about time during which allocations in the Java heap occurred,
  // how large these allocations were in bytes, and an additional buffer.
  // The allocations should contain any amount of space made unusable for further
  // allocation, e.g. any waste caused by TLAB allocation, space at the end of
  // humongous objects that can not be used for allocation, etc.
  // Together with the target occupancy, this additional buffer should contain the
  // difference between old gen size and total heap size at the start of reclamation,
  // and space required for that reclamation.
 // 这个函数用于更新 Java 堆中分配的信息,包括分配时间和额外的缓冲区大小。
  virtual void update_allocation_info(double allocation_time_s, size_t additional_buffer_size);
  // Update the time spent in the mutator beginning from the end of concurrent start to
  // the first mixed gc.
  //这是一个纯虚函数,子类需要实现它来更新最近一次并发标记阶段的持续时间。
  virtual void update_marking_length(double marking_length_s) = 0;
//这个函数用于打印 IHOP 控制器的相关信息。
  virtual void print();
  //这个函数用于向 G1 跟踪器发送 IHOP 统计信息。
  virtual void send_trace_event(G1NewTracer* tracer);
};

// The returned concurrent mark starting occupancy threshold is a fixed value
// relative to the maximum heap size.
class G1StaticIHOPControl : public G1IHOPControl {
  // Most recent mutator time between the end of concurrent mark to the start of the
  // first mixed gc.
  double _last_marking_length_s;
 protected:
  double last_marking_length_s() const { return _last_marking_length_s; }
 public:
  G1StaticIHOPControl(double ihop_percent, G1OldGenAllocationTracker const* old_gen_alloc_tracker);

  size_t get_conc_mark_start_threshold() {
    guarantee(_target_occupancy > 0, "Target occupancy must have been initialized.");
    return (size_t) (_initial_ihop_percent * _target_occupancy / 100.0);
  }

  virtual void update_marking_length(double marking_length_s) {
   assert(marking_length_s > 0.0, "Marking length must be larger than zero but is %.3f", marking_length_s);
    _last_marking_length_s = marking_length_s;
  }
};

// This algorithm tries to return a concurrent mark starting occupancy value that
// makes sure that during marking the given target occupancy is never exceeded,
// based on predictions of current allocation rate and time periods between
// concurrent start and the first mixed gc.
//实现了一个自适应的 IHOP 控制算法,会根据运行时数据动态调整 IHOP 值。它使用了预测器和统计数据来计算合适的 IHOP 值,并考虑了堆的限制因素。
//它会返回一个并发标记起始占用率值，基于当前分配速率和并发启动到第一个混合gc之间的时间来进行
class G1AdaptiveIHOPControl : public G1IHOPControl {
  size_t _heap_reserve_percent; // Percentage of maximum heap capacity we should avoid to touch
  size_t _heap_waste_percent;   // Percentage of free heap that should be considered as waste.

//一个指向 G1Predictions 对象的指针,用于进行预测
  const G1Predictions * _predictor;
//一个 TruncatedSeq 对象,用于存储最近的并发标记持续时间。
  TruncatedSeq _marking_times_s;
  //一个 TruncatedSeq 对象,用于存储最近的老年代分配速率。
  TruncatedSeq _allocation_rate_s;

  // The most recent unrestrained size of the young gen. This is used as an additional
  // factor in the calculation of the threshold, as the threshold is based on
  // non-young gen occupancy at the end of GC. For the IHOP threshold, we need to
  // consider the young gen size during that time too.
  // Since we cannot know what young gen sizes are used in the future, we will just
  // use the current one. We expect that this one will be one with a fairly large size,
  // as there is no marking or mixed gc that could impact its size too much.
  //最近一次未受限制的 young 代大小,用于计算阈值。 这用作计算阈值的附加因素，因为阈值基于 GC 结束时非年轻代的占用率。对于 IHOP 阈值，我们也需要
  //考虑该时间内的年轻代大小。
  size_t _last_unrestrained_young_size;

   // Get a new prediction bounded below by zero from the given sequence.
   //从给定序列中获取一个低于零的新预测
  double predict(TruncatedSeq const* seq) const;
  // 检测是否有足够的数据来进行预测
  bool have_enough_data_for_prediction() const;

  // The "actual" target threshold the algorithm wants to keep during and at the
  // end of marking. This is typically lower than the requested threshold, as the
  // algorithm needs to consider restrictions by the environment.
  //算法希望在标记期间和标记结束时保持的“实际”目标阈值。这通常低于请求的阈值，因为算法需要考虑环境的限制
  //返回的是实际的目标阈值
  size_t actual_target_threshold() const;

  // This method calculates the old gen allocation rate based on the net survived
  // bytes that are allocated in the old generation in the last mutator period.
  //// 此方法根据上一个改变器周期中在老生代中分配的净存活字节数来计算老生代分配率。
  double last_mutator_period_old_allocation_rate() const;
 protected:
 //返回 _marking_times_s 中最近的值。
  virtual double last_marking_length_s() const { return _marking_times_s.last(); }
 public:
  G1AdaptiveIHOPControl(double ihop_percent,
                        G1OldGenAllocationTracker const* old_gen_alloc_tracker,
                        G1Predictions const* predictor,
                        size_t heap_reserve_percent, // The percentage of total heap capacity that should not be tapped into.
                        size_t heap_waste_percent);  // The percentage of the free space in the heap that we think is not usable for allocation.

//实现了父类的纯虚函数,根据预测的分配速率和标记持续时间计算并发标记开始阈值。
  virtual size_t get_conc_mark_start_threshold();

// 实现了父类的虚函数,更新分配信息并更新 _allocation_rate_s。
  virtual void update_allocation_info(double allocation_time_s, size_t additional_buffer_size);
  //实现了父类的纯虚函数,更新 _marking_times_s。
  virtual void update_marking_length(double marking_length_s);
 //重写了父类的虚函数,打印更多的自适应 IHOP 信息。
  virtual void print();
  //重写了父类的虚函数,发送更多的自适应 IHOP 统计信息。
  virtual void send_trace_event(G1NewTracer* tracer);
};

#endif // SHARE_GC_G1_G1IHOPCONTROL_HPP
