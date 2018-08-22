/*
 * Copyright 2018, Oath Inc. Licensed under the terms of the
 * Apache License 2.0. See LICENSE file at the project root for terms.
 */

#ifndef CPC_SKETCH_HPP_
#define CPC_SKETCH_HPP_

#include <iostream>
#include <memory>

extern "C" {

#include "fm85.h"
#include "fm85Compression.h"
#include "iconEstimator.h"

}

#include "MurmurHash3.h"

namespace datasketches {

/*
 * High performance C++ implementation of Compressed Probabilistic Counting sketch
 *
 * author Kevin Lang
 * author Alexander Saydakov
 */

class cpc_sketch {
  public:

    explicit cpc_sketch(uint8_t lg_k) {
      fm85Init();
      // TODO: check lg_k
      state = fm85Make(lg_k);
    }

    cpc_sketch(const cpc_sketch& other) {
      state = fm85Copy(other.state);
    }

    cpc_sketch& operator=(cpc_sketch other) {
      std::swap(state, other.state); // @suppress("Invalid arguments")
      return *this;
    }

    ~cpc_sketch() {
      fm85Free(state);
    }

    bool is_empty() const {
      return state->numCoupons == 0;
    }

    double get_estimate() const {
      if (!state->mergeFlag) return getHIPEstimate(state);
      return getIconEstimate(state->lgK, state->numCoupons);
    }

    double get_lower_bound(int num_std_dev) {
      return 0; // TODO
    }

    double get_upper_bound(int num_std_dev) {
      return 0; // TODO
    }

    void update(uint64_t value) {
      uint64_t hash[2];
      MurmurHash3_x64_128(&value, sizeof(value), DEFAULT_SEED, &hash);
      fm85Update(state, hash[0], hash[1]);
    }

    void serialize(std::ostream& os) const {
      FM85* compressed = fm85Compress(state);
      const uint8_t preamble_ints(get_preamble_ints(compressed));
      os.write((char*)&preamble_ints, sizeof(preamble_ints));
      const uint8_t serial_version(SERIAL_VERSION);
      os.write((char*)&serial_version, sizeof(serial_version));
      const uint8_t family(FAMILY);
      os.write((char*)&family, sizeof(family));
      const uint8_t lg_k(compressed->lgK);
      os.write((char*)&lg_k, sizeof(lg_k));
      const uint8_t first_interesting_column(compressed->firstInterestingColumn);
      os.write((char*)&first_interesting_column, sizeof(first_interesting_column));
      const bool has_hip(!compressed->mergeFlag);
      const bool has_table(compressed->compressedSurprisingValues != nullptr);
      const bool has_window(compressed->compressedWindow != nullptr);
      const uint8_t flags_byte(
          (has_hip ? 1 << flags::HAS_HIP : 0)
        | (has_table ? 1 << flags::HAS_TABLE : 0)
        | (has_window ? 1 << flags::HAS_WINDOW : 0)
      );
      os.write((char*)&flags_byte, sizeof(flags_byte));
      const uint16_t seed_hash(0); // unused for now
      os.write((char*)&seed_hash, sizeof(seed_hash));
      if (!is_empty()) {
        const uint32_t num_coupons(compressed->numCoupons);
        os.write((char*)&num_coupons, sizeof(num_coupons));
        if (has_table && has_window) {
          // if there is no window it is the same as number of coupons
          const uint32_t num_values(compressed->numCompressedSurprisingValues);
          os.write((char*)&num_values, sizeof(num_values));
          // HIP values are at the same offset because of alignment, which can be in two different places in the sequence of fields
          // this is the first HIP decision point
          if (has_hip) write_hip(compressed, os);
        }
        if (has_table) {
          const uint32_t csv_length(compressed->csvLength);
          os.write((char*)&csv_length, sizeof(csv_length));
        }
        if (has_window) {
          const uint32_t cw_length(compressed->cwLength);
          os.write((char*)&cw_length, sizeof(cw_length));
        }
        // this is the second HIP decision point
        if (has_hip && !(has_table && has_window)) write_hip(compressed, os);
        if (has_table) {
          os.write((char*)compressed->compressedSurprisingValues, compressed->csvLength * sizeof(uint32_t));
        }
        if (has_window) {
          os.write((char*)compressed->compressedWindow, compressed->cwLength * sizeof(uint32_t));
        }
      }
      fm85Free(compressed);
    }

    static std::unique_ptr<cpc_sketch> deserialize(std::istream& is) {
      uint8_t preamble_ints;
      is.read((char*)&preamble_ints, sizeof(preamble_ints));
      uint8_t serial_version;
      is.read((char*)&serial_version, sizeof(serial_version));
      uint8_t family_id;
      is.read((char*)&family_id, sizeof(family_id));
      uint8_t lg_k;
      is.read((char*)&lg_k, sizeof(lg_k));
      uint8_t first_interesting_column;
      is.read((char*)&first_interesting_column, sizeof(first_interesting_column));
      uint8_t flags_byte;
      is.read((char*)&flags_byte, sizeof(flags_byte));
      uint16_t seed_hash;
      is.read((char*)&seed_hash, sizeof(seed_hash));
      const bool has_hip(flags_byte & (1 << flags::HAS_HIP));
      const bool has_table(flags_byte & (1 << flags::HAS_TABLE));
      const bool has_window(flags_byte & (1 << flags::HAS_WINDOW));
      FM85 compressed;
      compressed.isCompressed = 1;
      compressed.mergeFlag = has_hip ? 0 : 1;
      compressed.lgK = lg_k;
      compressed.firstInterestingColumn = first_interesting_column;
      compressed.numCoupons = 0;
      compressed.numCompressedSurprisingValues = 0;
      compressed.kxp = 1 << lg_k;
      compressed.hipEstAccum = 0;
      compressed.hipErrAccum = 0;
      compressed.csvLength = 0;
      compressed.cwLength = 0;
      compressed.compressedSurprisingValues = nullptr;
      compressed.compressedWindow = nullptr;
      compressed.surprisingValueTable = nullptr;
      compressed.slidingWindow = nullptr;
      if (has_table || has_window) {
        uint32_t num_coupons;
        is.read((char*)&num_coupons, sizeof(num_coupons));
        compressed.numCoupons = num_coupons;
        if (has_table && has_window) {
          uint32_t num_values;
          is.read((char*)&num_values, sizeof(num_values));
          compressed.numCompressedSurprisingValues = num_values;
          if (has_hip) read_hip(&compressed, is);
        }
        if (has_table) {
          uint32_t csv_length;
          is.read((char*)&csv_length, sizeof(csv_length));
          compressed.csvLength = csv_length;
        }
        if (has_window) {
          uint32_t cw_length;
          is.read((char*)&cw_length, sizeof(cw_length));
          compressed.cwLength = cw_length;
        }
        if (has_hip && !(has_table && has_window)) read_hip(&compressed, is);
        if (has_table) {
          compressed.compressedSurprisingValues = new uint32_t[compressed.csvLength];
          is.read((char*)compressed.compressedSurprisingValues, compressed.csvLength * sizeof(uint32_t));
        }
        if (has_window) {
          compressed.compressedWindow = new uint32_t[compressed.cwLength];
          is.read((char*)compressed.compressedWindow, compressed.cwLength * sizeof(uint32_t));
        }
        if (!has_window) compressed.numCompressedSurprisingValues = compressed.numCoupons;
      }
      compressed.windowOffset = determineCorrectOffset(compressed.lgK, compressed.numCoupons);

      uint8_t expected_preamble_ints(get_preamble_ints(&compressed));
      if (preamble_ints != expected_preamble_ints) {
        throw std::invalid_argument("Possible corruption: preamble ints: expected " + std::to_string(expected_preamble_ints)
            + ", got " + std::to_string(preamble_ints));
      }
      if (serial_version != SERIAL_VERSION) {
        throw std::invalid_argument("Possible corruption: serial version: expected "
            + std::to_string(SERIAL_VERSION) + ", got " + std::to_string(serial_version));
      }
      if (family_id != FAMILY) {
        throw std::invalid_argument("Possible corruption: family: expected "
            + std::to_string(FAMILY) + ", got " + std::to_string(family_id));
      }
      FM85* uncompressed = fm85Uncompress(&compressed);
      delete [] compressed.compressedSurprisingValues;
      delete [] compressed.compressedWindow;
      std::unique_ptr<cpc_sketch> sketch_ptr(new cpc_sketch(uncompressed));
      return std::move(sketch_ptr);
    }

    // for debugging
    uint64_t get_num_coupons() const {
      return state->numCoupons;
    }

    friend std::ostream& operator<<(std::ostream& os, cpc_sketch const& sketch);

    friend class cpc_union;

  private:
    static const uint64_t DEFAULT_SEED = 9001;
    static const uint8_t SERIAL_VERSION = 1;
    static const uint8_t FAMILY = 16;

    enum flags { HAS_HIP, HAS_TABLE, HAS_WINDOW };

    FM85* state;

    // for deserialization and cpc_union::get_result()
    cpc_sketch(FM85* state) {
      this->state = state;
    }

    static uint8_t get_preamble_ints(FM85* state) {
      uint8_t preamble_ints(2);
      if (state->numCoupons > 0) {
        preamble_ints += 1; // number of coupons
        if (!state->mergeFlag) {
          preamble_ints += 4; // HIP
        }
        if (state->compressedSurprisingValues != nullptr) {
          preamble_ints += 1; // table length
          // number of values (if there is no window it is the same as number of coupons)
          if (state->compressedWindow != nullptr) {
            preamble_ints += 1;
          }
        }
        if (state->compressedWindow != nullptr) {
          preamble_ints += 1; // window length
        }
      }
      return preamble_ints;
    }

    static void write_hip(FM85* state, std::ostream& os) {
      os.write((char*)&state->kxp, sizeof(FM85::kxp));
      os.write((char*)&state->hipEstAccum, sizeof(FM85::hipEstAccum));
    }

    static void read_hip(FM85* state, std::istream& is) {
      is.read((char*)&state->kxp, sizeof(FM85::kxp));
      is.read((char*)&state->hipEstAccum, sizeof(FM85::hipEstAccum));
    }

};

} /* namespace datasketches */

#endif
