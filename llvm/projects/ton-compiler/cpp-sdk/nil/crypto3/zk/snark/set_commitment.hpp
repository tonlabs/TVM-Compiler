//---------------------------------------------------------------------------//
// Copyright (c) 2018-2020 Mikhail Komarov <nemo@nil.foundation>
// Copyright (c) 2020 Nikita Kaskov <nbering@nil.foundation>
//
// Distributed under the Boost Software License, Version 1.0
// See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt
//---------------------------------------------------------------------------//

#ifndef CRYPTO3_ZK_SNARK_SET_COMMITMENT_HPP
#define CRYPTO3_ZK_SNARK_SET_COMMITMENT_HPP

#include <nil/crypto3/zk/snark/merkle_tree.hpp>
#include <nil/crypto3/zk/snark/components/hashes/hash_io.hpp>

namespace nil {
    namespace crypto3 {
        namespace zk {
            namespace snark {

                typedef std::vector<bool> set_commitment;

                struct set_membership_proof {
                    std::size_t address;
                    merkle_authentication_path merkle_path;

                    bool operator==(const set_membership_proof &other) const {
                        return (this->address == other.address && this->merkle_path == other.merkle_path);
                    }

                    std::size_t size_in_bits() const {
                        if (merkle_path.empty()) {
                            return (8 * sizeof(address));
                        } else {
                            return (8 * sizeof(address) + merkle_path[0].size() * merkle_path.size());
                        }
                    }
                };

                template<typename Hash>
                class set_commitment_accumulator {
                private:
                    std::shared_ptr<merkle_tree<Hash>> tree;
                    std::map<std::vector<bool>, std::size_t> hash_to_pos;

                public:
                    std::size_t depth;
                    std::size_t digest_size;
                    std::size_t value_size;

                    set_commitment_accumulator(const std::size_t max_entries, const std::size_t value_size = 0) :
                        value_size(value_size) {
                        depth = static_cast<std::size_t>(std::ceil(std::log2(max_entries)));
                        digest_size = Hash::get_digest_len();

                        tree.reset(new merkle_tree<Hash>(depth, digest_size));
                    }

                    void add(const std::vector<bool> &value) {
                        assert(value_size == 0 || value.size() == value_size);
                        const std::vector<bool> hash = Hash::get_hash(value);
                        if (hash_to_pos.find(hash) == hash_to_pos.end()) {
                            const std::size_t pos = hash_to_pos.size();
                            tree->set_value(pos, hash);
                            hash_to_pos[hash] = pos;
                        }
                    }

                    bool is_in_set(const std::vector<bool> &value) const {
                        assert(value_size == 0 || value.size() == value_size);
                        const std::vector<bool> hash = Hash::get_hash(value);
                        return (hash_to_pos.find(hash) != hash_to_pos.end());
                    }

                    set_commitment get_commitment() const {
                        return tree->get_root();
                    }

                    set_membership_proof get_membership_proof(const std::vector<bool> &value) const {
                        const std::vector<bool> hash = Hash::get_hash(value);
                        auto it = hash_to_pos.find(hash);
                        assert(it != hash_to_pos.end());

                        set_membership_proof proof;
                        proof.address = it->second;
                        proof.merkle_path = tree->get_path(it->second);

                        return proof;
                    }
                };

            }    // namespace snark
        }        // namespace zk
    }            // namespace crypto3
}    // namespace nil

#endif    // CRYPTO3_ZK_SNARK_SET_COMMITMENT_HPP
