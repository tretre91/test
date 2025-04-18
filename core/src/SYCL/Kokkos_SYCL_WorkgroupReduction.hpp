//@HEADER
// ************************************************************************
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
// Part of Kokkos, under the Apache License v2.0 with LLVM Exceptions.
// See https://kokkos.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//@HEADER

#ifndef KOKKOS_SYCL_WORKGROUP_REDUCTION_HPP
#define KOKKOS_SYCL_WORKGROUP_REDUCTION_HPP

#include <Kokkos_Macros.hpp>

namespace Kokkos::Impl::SYCLReduction {

template <int N>
struct TrivialWrapper {
  std::byte array[N];
};

// shuffle down
template <typename T>
T shift_group_left(sycl::sub_group sg, T x,
                   sycl::sub_group::linear_id_type delta) {
  if constexpr (std::is_trivially_copyable_v<T>)
    return sycl::shift_group_left(sg, x, delta);
  else {
    auto tmp = sycl::shift_group_left(
        sg, reinterpret_cast<TrivialWrapper<sizeof(T)>&>(x), delta);
    return reinterpret_cast<T&>(tmp);
  }
}

// shuffle up
template <typename T>
T shift_group_right(sycl::sub_group sg, T x,
                    sycl::sub_group::linear_id_type delta) {
  if constexpr (std::is_trivially_copyable_v<T>)
    return sycl::shift_group_right(sg, x, delta);
  else {
    auto tmp = sycl::shift_group_right(
        sg, reinterpret_cast<TrivialWrapper<sizeof(T)>&>(x), delta);
    return reinterpret_cast<T&>(tmp);
  }
}

// shuffle
template <typename T>
T select_from_group(sycl::sub_group sg, T x,
                    sycl::sub_group::id_type remote_local_id) {
  if constexpr (std::is_trivially_copyable_v<T>)
    return sycl::select_from_group(sg, x, remote_local_id);
  else {
    auto tmp = sycl::select_from_group(
        sg, reinterpret_cast<TrivialWrapper<sizeof(T)>&>(x), remote_local_id);
    return reinterpret_cast<T&>(tmp);
  }
}

// FIXME_SYCL For some types, shuffle reductions are competitive with local
// memory reductions but they are significantly slower for the value type used
// in combined reductions with multiple double arguments.
template <class ReducerType>
inline constexpr bool use_shuffle_based_algorithm = false;
// std::is_reference_v<typename ReducerType::reference_type>;

template <typename ValueType, typename ReducerType, int dim>
std::enable_if_t<!use_shuffle_based_algorithm<ReducerType>> workgroup_reduction(
    sycl::nd_item<dim>& item, sycl::local_accessor<ValueType> local_mem,
    sycl_device_ptr<ValueType> results_ptr,
    sycl::global_ptr<ValueType> device_accessible_result_ptr,
    const unsigned int value_count_, const ReducerType& final_reducer,
    bool final, unsigned int max_size) {
  const unsigned int value_count =
      std::is_reference_v<typename ReducerType::reference_type> ? 1
                                                                : value_count_;
  const int local_id = item.get_local_linear_id();

  // Perform the actual workgroup reduction in each subgroup
  // separately.
  auto sg            = item.get_sub_group();
  auto* result       = &local_mem[local_id * value_count];
  const int id_in_sg = sg.get_local_id()[0];
  const auto local_range =
      std::min<unsigned int>(sg.get_local_range()[0], max_size);
  const auto upper_stride_bound =
      std::min<unsigned int>(local_range - id_in_sg, max_size - local_id);
  for (unsigned int stride = 1; stride < local_range; stride <<= 1) {
    if (stride < upper_stride_bound)
      final_reducer.join(result, &local_mem[(local_id + stride) * value_count]);
    sycl::group_barrier(sg);
  }
  sycl::group_barrier(item.get_group());

  // Do the final reduction only using the first subgroup.
  if (sg.get_group_id()[0] == 0) {
    const unsigned int n_subgroups = sg.get_group_range()[0];
    const int max_subgroup_size    = sg.get_max_local_range()[0];
    auto* result_ = &local_mem[id_in_sg * max_subgroup_size * value_count];
    // In case the number of subgroup results is larger than the range of
    // the first subgroup, we first combine the items with a higher
    // index.
    for (unsigned int offset = local_range;
         offset < std::min(n_subgroups, max_size); offset += local_range)
      if (id_in_sg + offset < n_subgroups)
        final_reducer.join(
            result_,
            &local_mem[(id_in_sg + offset) * max_subgroup_size * value_count]);
    sycl::group_barrier(sg);

    // Then, we proceed as before.
    for (unsigned int stride = 1; stride < local_range; stride <<= 1) {
      if (id_in_sg + stride < n_subgroups)
        final_reducer.join(
            result_,
            &local_mem[(id_in_sg + stride) * max_subgroup_size * value_count]);
      sycl::group_barrier(sg);
    }

    // Finally, we copy the workgroup results back to global memory
    // to be used in the next iteration. If this is the last
    // iteration, i.e., there is only one workgroup also call
    // final() if necessary.
    if (id_in_sg == 0) {
      if (final) {
        final_reducer.final(&local_mem[0]);
        if (device_accessible_result_ptr != nullptr)
          final_reducer.copy(&device_accessible_result_ptr[0], &local_mem[0]);
        else
          final_reducer.copy(&results_ptr[0], &local_mem[0]);
      } else
        final_reducer.copy(
            &results_ptr[(item.get_group_linear_id()) * value_count],
            &local_mem[0]);
    }
  }
}

template <typename ValueType, typename ReducerType, int dim>
std::enable_if_t<use_shuffle_based_algorithm<ReducerType>> workgroup_reduction(
    sycl::nd_item<dim>& item, sycl::local_accessor<ValueType> local_mem,
    ValueType local_value, sycl_device_ptr<ValueType> results_ptr,
    sycl::global_ptr<ValueType> device_accessible_result_ptr,
    const ReducerType& final_reducer, bool final, unsigned int max_size) {
  const auto local_id = item.get_local_linear_id();

  // Perform the actual workgroup reduction in each subgroup
  // separately.
  auto sg               = item.get_sub_group();
  const int id_in_sg    = sg.get_local_id()[0];
  const int local_range = std::min<int>(sg.get_local_range()[0], max_size);

  const auto upper_stride_bound =
      std::min<int>(local_range - id_in_sg, max_size - local_id);
#if defined(KOKKOS_ARCH_INTEL_GPU) || defined(KOKKOS_IMPL_ARCH_NVIDIA_GPU)
  auto shuffle_combine = [&](int stride) {
    if (stride < local_range) {
      auto tmp = Kokkos::Impl::SYCLReduction::shift_group_left(sg, local_value,
                                                               stride);
      if (stride < upper_stride_bound) final_reducer.join(&local_value, &tmp);
    }
  };
  shuffle_combine(1);
  shuffle_combine(2);
  shuffle_combine(4);
  shuffle_combine(8);
  shuffle_combine(16);
  KOKKOS_ASSERT(local_range <= 32);
#else
  for (unsigned int stride = 1; stride < local_range; stride <<= 1) {
    auto tmp =
        Kokkos::Impl::SYCLReduction::shift_group_left(sg, local_value, stride);
    if (stride < upper_stride_bound) final_reducer.join(&local_value, &tmp);
  }
#endif

  // Copy the subgroup results into the first positions of the
  // reduction array.
  const int max_subgroup_size = sg.get_max_local_range()[0];
  const int n_active_subgroups =
      (max_size + max_subgroup_size - 1) / max_subgroup_size;
  const int sg_group_id = sg.get_group_id()[0];
  if (id_in_sg == 0 && sg_group_id <= n_active_subgroups)
    local_mem[sg_group_id] = local_value;

  sycl::group_barrier(item.get_group());

  // Do the final reduction only using the first subgroup.
  if (sg.get_group_id()[0] == 0) {
    auto sg_value = local_mem[id_in_sg < n_active_subgroups ? id_in_sg : 0];

    // In case the number of subgroups is larger than the range of
    // the first subgroup, we first combine the items with a higher
    // index.
    if (n_active_subgroups > local_range) {
      for (int offset = local_range; offset < n_active_subgroups;
           offset += local_range)
        if (id_in_sg + offset < n_active_subgroups) {
          final_reducer.join(&sg_value, &local_mem[(id_in_sg + offset)]);
        }
      sycl::group_barrier(sg);
    }

    // Then, we proceed as before.
#if defined(KOKKOS_ARCH_INTEL_GPU) || defined(KOKKOS_IMPL_ARCH_NVIDIA_GPU)
    auto shuffle_combine_sg = [&](int stride) {
      if (stride < local_range) {
        auto tmp =
            Kokkos::Impl::SYCLReduction::shift_group_left(sg, sg_value, stride);
        if (id_in_sg + stride < n_active_subgroups)
          final_reducer.join(&sg_value, &tmp);
      }
    };
    shuffle_combine_sg(1);
    shuffle_combine_sg(2);
    shuffle_combine_sg(4);
    shuffle_combine_sg(8);
    shuffle_combine_sg(16);
    KOKKOS_ASSERT(local_range <= 32);
#else
    for (unsigned int stride = 1; stride < local_range; stride <<= 1) {
      auto tmp =
          Kokkos::Impl::SYCLReduction::shift_group_left(sg, sg_value, stride);
      if (id_in_sg + stride < n_active_subgroups)
        final_reducer.join(&sg_value, &tmp);
    }
#endif

    // Finally, we copy the workgroup results back to global memory
    // to be used in the next iteration. If this is the last
    // iteration, i.e., there is only one workgroup also call
    // final() if necessary.
    if (id_in_sg == 0) {
      if (final) {
        final_reducer.final(&sg_value);
        if (device_accessible_result_ptr != nullptr)
          device_accessible_result_ptr[0] = sg_value;
        else
          results_ptr[0] = sg_value;
      } else
        results_ptr[(item.get_group_linear_id())] = sg_value;
    }
  }
}

}  // namespace Kokkos::Impl::SYCLReduction

#endif /* KOKKOS_SYCL_WORKGROUP_REDUCTION_HPP */
