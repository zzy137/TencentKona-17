/*
 * Copyright (c) 2001, 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1COLLECTIONSETCHOOSER_HPP
#define SHARE_GC_G1_G1COLLECTIONSETCHOOSER_HPP

#include "gc/g1/heapRegion.hpp"
#include "memory/allocation.hpp"
#include "runtime/globals.hpp"

class G1CollectionSetCandidates;
// 一个工作线程组,用于并行处理集合集候选对象的构建。
class WorkGang;

// Helper class to calculate collection set candidates, and containing some related
// methods.
class G1CollectionSetChooser : public AllStatic {
  static uint calculate_work_chunk_size(uint num_workers, uint num_regions);

  // Remove regions in the collection set candidates as long as the G1HeapWastePercent
  // criteria is met. Keep at least the minimum amount of old regions to guarantee
  // some progress.
  //根据 G1HeapWastePercent 标准从集合集候选对象中删除区域,同时确保至少有一定数量的老年代区域。
  static void prune(G1CollectionSetCandidates* candidates);
public:
//返回在混合 GC 期间考虑纳入集合集的区域的存活字节阈值。
  static size_t mixed_gc_live_threshold_bytes() {
    return HeapRegion::GrainBytes * (size_t) G1MixedGCLiveThresholdPercent / 100;
  }

  static bool region_occupancy_low_enough_for_evac(size_t live_bytes) {
    return live_bytes < mixed_gc_live_threshold_bytes();
  }

  // Determine whether to add the given region to the collection set candidates or
  // not. Currently, we skip pinned regions and regions whose live
  // bytes are over the threshold. Humongous regions may be reclaimed during cleanup.
  // Regions also need a complete remembered set to be a candidate.
  //它会跳过 pinned 区域和存活字节超过阈值的区域。
  //巨型区域可能会在清理过程中被回收。
  //区域还需要有一个完整的 remembered set 才能成为候选对象。
  //它负责根据一些预定义的条件来决定是否将一个给定的区域添加到集合集候选对象中。
  //这些条件包括区域的状态(pinned 或巨型)、存活字节数以及 remembered set 的完整性。
  static bool should_add(HeapRegion* hr);

  // Build and return set of collection set candidates sorted by decreasing gc
  // efficiency.
  // 这个方法用于构建并返回一个按 GC 效率降序排序的集合集候选对象。
  // max_num_regions 最大区域数量,用于限制集合集候选对象的大小。
  //它负责根据各种条件(如区域状态、存活字节数等)选择并构建集合集候选对象,然后按 GC 效率进行排序。
  //可以确保在下一个 GC 周期中,集合集包含了那些 GC 效率最高的区域,从而提高 G1 垃圾收集的整体性能。
  static G1CollectionSetCandidates* build(WorkGang* workers, uint max_num_regions);
};

#endif // SHARE_GC_G1_G1COLLECTIONSETCHOOSER_HPP
