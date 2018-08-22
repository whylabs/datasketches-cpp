/*
 * Copyright 2018, Oath Inc. Licensed under the terms of the
 * Apache License 2.0. See LICENSE file at the project root for terms.
 */

#ifndef CPC_UNION_HPP_
#define CPC_UNION_HPP_

extern "C" {

#include "fm85Merging.h"

}

#include "cpc_sketch.hpp"

namespace datasketches {

/*
 * High performance C++ implementation of Compressed Probabilistic Counting sketch
 *
 * author Kevin Lang
 * author Alexander Saydakov
 */

UG85* ug85Copy(UG85* other) {
  UG85* copy(new UG85(*other));
  if (other->accumulator != nullptr) copy->accumulator = fm85Copy(other->accumulator);
  if (other->bitMatrix != nullptr) {
    uint32_t k = 1 << copy->lgK;
    copy->bitMatrix = (U64 *) malloc ((size_t) (k * sizeof(U64)));
    std::copy(&other->bitMatrix[0], &other->bitMatrix[k], copy->bitMatrix);
  }
  return copy;
}

class cpc_union {
  public:
    explicit cpc_union(uint8_t lg_k) {
      fm85Init();
      // TODO: check lg_k
      state = ug85Make(lg_k);
    }

    cpc_union(const cpc_union& other) {
      state = ug85Copy(other.state);
    }

    cpc_union& operator=(cpc_union other) {
      std::swap(state, other.state); // @suppress("Invalid arguments")
      return *this;
    }

    ~cpc_union() {
      ug85Free(state);
    }

    void update(const cpc_sketch& sketch) {
      ug85MergeInto(state, sketch.state);
    }

    std::unique_ptr<cpc_sketch> get_result() const {
      std::unique_ptr<cpc_sketch> sketch(new cpc_sketch(ug85GetResult(state)));
      return std::move(sketch);
    }

  private:
    UG85* state;
};

} /* namespace datasketches */

#endif
