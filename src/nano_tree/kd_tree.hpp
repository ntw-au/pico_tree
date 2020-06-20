#pragma once

#include <algorithm>
#include <cassert>
#include <numeric>

#include "core.hpp"

namespace nano_tree {

namespace internal {

//! KdTree search visitor for finding a single nearest neighbor.
template <typename Index, typename Scalar>
class SearchNn {
 public:
  SearchNn() : min_{std::numeric_limits<Scalar>::max()} {}

  //! Visit current point.
  inline void operator()(Index const idx, Scalar const d) {
    idx_ = idx;
    min_ = d;
  }

  //! Maximum search distance with respect to the query point.
  inline Scalar max() const { return min_; }

  //! Returns the current nearest.
  inline std::pair<Index, Scalar> nearest() const {
    return std::make_pair(idx_, min_);
  }

 private:
  Index idx_;
  Scalar min_;
};

//! KdTree search visitor for finding k nearest neighbors.
template <typename Index, typename Scalar>
class SearchKnn {
 public:
  SearchKnn(Index const k, std::vector<std::pair<Index, Scalar>>* knn)
      : k_{k}, count_{0}, knn_{*knn} {
    knn_.resize(k_);
    // Initial search distance.
    knn_[k_ - 1].second = std::numeric_limits<Scalar>::max();
  }

  //! \private
  ~SearchKnn() {
    // If we couldn't find k neighbors the container gets resized to the amount
    // of points found.
    // Making a container smaller doesn't change the capacity and shouldn't
    // invalidate the content.
    knn_.resize(count_);
  }

  //! Visit current point.
  inline void operator()(Index const idx, Scalar const d) {
    if (count_ < k_) {
      count_++;
    }

    InsertSorted(idx, d);
  }

  //! Maximum search distance with respect to the query point.
  inline Scalar max() const { return knn_[k_ - 1].second; }

 private:
  //! \brief Inserts an element in O(n) time while keeping the vector the same
  //! length by replacing the last (furthest) element.
  //! \details It starts at the 2nd last element and moves to the front of the
  //! vector until the input distance \p d is no longer smaller than the
  //! distance being compared. While traversing the vector, each element gets
  //! copied 1 index upwards (towards end of the vector). This means we have k
  //! comparisons and k copies, where k is the amount of elements checked. This
  //! seems to dominate the Knn search time. Alternative strategies have been
  //! attempted:
  //!  * std::vector::insert(std::lower_bound).
  //!  * std::push_heap(std::vector) and std::pop_heap(std::vector).
  inline void InsertSorted(Index const idx, Scalar const d) {
    Index i;
    for (i = count_ - 1; i > 0; --i) {
      if (knn_[i - 1].second > d) {
        knn_[i] = knn_[i - 1];
      } else {
        break;
      }
    }
    // We update the inserted element outside of the loop. This is done for the
    // case where we didn't break, simply reaching the end of the loop. For the
    // last element (first in the list) we can't enter the "else" clause.
    knn_[i] = std::make_pair(idx, d);
  }

  Index const k_;
  Index count_;
  std::vector<std::pair<Index, Scalar>>& knn_;
};

//! KdTree search visitor for finding all neighbors within a radius.
template <typename Index, typename Scalar>
class SearchRadius {
 public:
  SearchRadius(Scalar const radius, std::vector<std::pair<Index, Scalar>>* n)
      : radius_{radius}, n_{*n} {
    n_.clear();
  }

  //! Visit current point.
  inline void operator()(Index const idx, Scalar const d) {
    n_.emplace_back(idx, d);
  }

  //! Maximum search distance with respect to the query point.
  inline Scalar max() const { return radius_; }

 private:
  Scalar const radius_;
  std::vector<std::pair<Index, Scalar>>& n_;
};

}  // namespace internal

//! \brief L2 metric that calculates the squared distance between two points.
//! Can be replaced by a custom metric.
template <typename Index, typename Scalar, int Dims, typename Points>
class MetricL2 {
 public:
  MetricL2(Points const& points) : points_{points} {}

  //! Calculates the difference between two points given a query point and an
  //! index to a point.
  //! \tparam P Point type.
  //! \param p Point.
  //! \param idx Index.
  template <typename P>
  inline Scalar operator()(P const& p, Index idx) const {
    Scalar d{};

    for (Index i = 0;
         i < internal::Dimensions<Dims>::Dims(points_.num_dimensions());
         ++i) {
      Scalar const v = points_(p, i) - points_(idx, i);
      d += v * v;
    }

    return d;
  }

 private:
  Points const& points_;
};

//! \brief A KdTree is a binary tree that partitions space using hyper planes.
//! \details https://en.wikipedia.org/wiki/K-d_tree
template <
    typename Index,
    typename Scalar,
    int Dims,
    typename Points,
    typename Metric = MetricL2<Index, Scalar, Dims, Points>>
class KdTree {
 private:
  //! KdTree Node.
  struct Node {
    //! Data is used to either store branch or leaf information. Which union
    //! member is used can be tested with IsBranch() or IsLeaf().
    union Data {
      //! Tree branch.
      struct Branch {
        Scalar split;
        Index dim;
      };

      //! Tree leaf.
      struct Leaf {
        Index begin_idx;
        Index end_idx;
      };

      Branch branch;
      Leaf leaf;
    };

    inline bool IsBranch() const { return left != nullptr && right != nullptr; }
    inline bool IsLeaf() const { return left == nullptr && right == nullptr; }

    Data data;
    Node* left;
    Node* right;
  };

 public:
  KdTree(Points const& points, Index const max_leaf_size)
      : points_{points},
        metric_{points_},
        dimensions_{points_.num_dimensions()},
        nodes_{
            internal::MaxNodesFromPoints(points_.num_points(), max_leaf_size)},
        indices_(points_.num_points()),
        root_{MakeTree(max_leaf_size)} {
    assert(points_.num_points() > 0);
  }

  //! Returns the nearest neighbor of point \p p in O(log n) time.
  //! \tparam P point type.
  template <typename P>
  inline std::pair<Index, Scalar> SearchNn(P const& p) const {
    internal::SearchNn<Index, Scalar> v;
    SearchNn(p, root_, &v);
    return v.nearest();
  }

  //! Returns the \p k nearest neighbors of point \p p .
  //! \tparam P point type.
  template <typename P>
  inline void SearchKnn(
      P const& p,
      Index const k,
      std::vector<std::pair<Index, Scalar>>* knn) const {
    internal::SearchKnn<Index, Scalar> v(k, knn);
    SearchNn(p, root_, &v);
  }

  //! Returns all neighbors to point \p p that are within squared
  //! radius \p radius .
  //! \tparam P point type.
  template <typename P>
  inline void SearchRadius(
      P const& p,
      Scalar const radius,
      std::vector<std::pair<Index, Scalar>>* n) const {
    internal::SearchRadius<Index, Scalar> v(radius, n);
    SearchNn(p, root_, &v);
  }

 private:
  //! Builds a tree given a \p max_leaf_size in O(n log n) time.
  inline Node* MakeTree(Index const max_leaf_size) {
    std::iota(indices_.begin(), indices_.end(), 0);
    return SplitIndices(max_leaf_size, 0, points_.num_points(), 0, &indices_);
  }

  //! Creates a tree node for a range of indices, splits the range in two and
  //! recursively does the same for each sub set of indices until the index
  //! range \p size is less than or equal to \p max_leaf_size .
  inline Node* SplitIndices(
      Index const max_leaf_size,
      Index const offset,
      Index const size,
      Index const dim,
      std::vector<Index>* p_indices) {
    std::vector<Index>& indices = *p_indices;
    Node* node = nodes_.MakeItem();
    //
    if (size <= max_leaf_size) {
      node->data.leaf.begin_idx = offset;
      node->data.leaf.end_idx = offset + size;
      node->left = nullptr;
      node->right = nullptr;
    } else {
      Points const& points = points_;
      Index const left_size = size / 2;
      Index const right_size = size - left_size;
      Index const split = offset + left_size;
      std::nth_element(
          indices.begin() + offset,
          indices.begin() + split,
          indices.begin() + offset + size,
          [&points, dim](Index const a, Index const b) -> bool {
            return points(a, dim) < points(b, dim);
          });

      Index const next_dim = ((dim + 1) < dimensions_) ? (dim + 1) : 0;
      node->data.branch.split = points(indices[split], dim);
      node->data.branch.dim = dim;
      node->left =
          SplitIndices(max_leaf_size, offset, left_size, next_dim, p_indices);
      node->right =
          SplitIndices(max_leaf_size, split, right_size, next_dim, p_indices);
    }

    return node;
  }

  //! Returns the nearest neighbor or neighbors of point \p p depending
  //! selection by visitor \p visitor for node \p node .
  template <typename P, typename V>
  inline void SearchNn(P const& p, Node const* const node, V* visitor) const {
    if (node->IsBranch()) {
      Scalar const v = points_(p, node->data.branch.dim);
      Scalar const d = v - node->data.branch.split;
      // Go left or right and then check if we should still go down the other
      // side based on the current minimum distance.
      if (v <= node->data.branch.split) {
        SearchNn(p, node->left, visitor);
        if (visitor->max() > d * d) {
          SearchNn(p, node->right, visitor);
        }
      } else {
        SearchNn(p, node->right, visitor);
        if (visitor->max() > d * d) {
          SearchNn(p, node->left, visitor);
        }
      }
    } else {
      // TODO If the indices are stored directly in the leaves, perhaps that is
      // faster than an index to an index.
      Scalar const max = visitor->max();
      for (Index i = node->data.leaf.begin_idx; i < node->data.leaf.end_idx;
           ++i) {
        Index const idx = indices_[i];
        Scalar const d = metric_(p, idx);
        if (max > d) {
          (*visitor)(idx, d);
        }
      }
    }
  }

  Points const& points_;
  Metric const metric_;
  Index const dimensions_;
  internal::ItemBuffer<Node> nodes_;
  std::vector<Index> indices_;
  Node* root_;
};

}  // namespace nano_tree