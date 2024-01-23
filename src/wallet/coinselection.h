// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_COINSELECTION_H
#define BITCOIN_WALLET_COINSELECTION_H

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <outputtype.h>
#include <primitives/transaction.h>
#include <random.h>
#include <timedata.h>
#include <util/system.h>
#include <util/check.h>

#include <optional>

namespace wallet {
//! lower bound for randomly-chosen target change amount
static constexpr CAmount CHANGE_LOWER{50000};
//! upper bound for randomly-chosen target change amount
static constexpr CAmount CHANGE_UPPER{1000000};
//! final minimum change amount after paying for fees
static const CAmount MIN_FINAL_CHANGE = MIN_TXOUT_AMOUNT;

/** A UTXO under consideration for use in funding a new transaction. */
struct COutput {
private:
    /** The output's value minus fees required to spend it.*/
    std::optional<CAmount> effective_value;

    /** The fee required to spend this output at the transaction's target feerate. */
    std::optional<CAmount> fee;

public:
    /** The outpoint identifying this UTXO */
    COutPoint outpoint;

    /** The output itself */
    CTxOut txout;

    /**
     * Depth in block chain.
     * If > 0: the tx is on chain and has this many confirmations.
     * If = 0: the tx is waiting confirmation.
     * If < 0: a conflicting tx is on chain and has this many confirmations. */
    int depth;

    /** Pre-computed estimated size of this output as a fully-signed input in a transaction. Can be -1 if it could not be calculated */
    int input_bytes;

    /** Whether we have the private keys to spend this output */
    bool spendable;

    /** Whether we know how to spend this output, ignoring the lack of keys */
    bool solvable;

    /**
     * Whether this output is considered safe to spend. Unconfirmed transactions
     * from outside keys and unconfirmed replacement transactions are considered
     * unsafe and will not be used to fund new spending transactions.
     */
    bool safe;

    /** The time of the transaction containing this output as determined by CWalletTx::nTimeSmart */
    int64_t time;

    /** Whether the transaction containing this output is sent from the owning wallet */
    bool from_me;

    /** The fee required to spend this output at the consolidation feerate. */
    CAmount long_term_fee{0};

    COutput(const COutPoint& outpoint, const CTxOut& txout, int depth, int input_bytes, bool spendable, bool solvable, bool safe, int64_t time, bool from_me)
        : outpoint{outpoint},
          txout{txout},
          depth{depth},
          input_bytes{input_bytes},
          spendable{spendable},
          solvable{solvable},
          safe{safe},
          time{time},
          from_me{from_me}
    {
       fee = input_bytes < 0 ? 0 : GetMinFee(input_bytes, TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()));
       effective_value = txout.nValue - fee.value();
    }

    COutput(const COutPoint& outpoint, const CTxOut& txout, int depth, int input_bytes, bool spendable, bool solvable, bool safe, int64_t time, bool from_me, const CAmount fees)
        : COutput(outpoint, txout, depth, input_bytes, spendable, solvable, safe, time, from_me)
    {
        // if input_bytes is unknown, then fees should be 0, if input_bytes is known, then the fees should be a positive integer or 0 (input_bytes known and fees = 0 only happens in the tests)
        assert((input_bytes < 0 && fees == 0) || (input_bytes > 0 && fees >= 0));
        fee = fees;
        effective_value = txout.nValue - fee.value();
    }

    std::string ToString() const;

    bool operator<(const COutput& rhs) const
    {
        return outpoint < rhs.outpoint;
    }

    CAmount GetFee() const
    {
        assert(fee.has_value());
        return fee.value();
    }

    CAmount GetEffectiveValue() const
    {
        assert(effective_value.has_value());
        return effective_value.value();
    }

    bool HasEffectiveValue() const { return effective_value.has_value(); }
};

/** Parameters for one iteration of Coin Selection. */
struct CoinSelectionParams {
    /** Randomness to use in the context of coin selection. */
    FastRandomContext& rng_fast;
    /** Size of a change output in bytes, determined by the output type. */
    size_t change_output_size = 0;
    /** Size of the input to spend a change output in virtual bytes. */
    size_t change_spend_size = 0;
    /** Mininmum change to target in Knapsack solver: select coins to cover the payment and
     * at least this value of change. */
    CAmount m_min_change_target{0};
    /** Minimum amount for creating a change output.
     * If change budget is smaller than min_change then we forgo creation of change output.
     */
    CAmount min_viable_change{0};
    /** Cost of creating the change output. */
    CAmount m_change_fee{0};
    /** Cost of creating the change output + cost of spending the change output in the future. */
    CAmount m_cost_of_change{0};
    /** Size of the transaction before coin selection, consisting of the header and recipient
     * output(s), excluding the inputs and change output(s). */
    size_t tx_noinputs_size = 0;
    /** Indicate that we are subtracting the fee from outputs */
    bool m_subtract_fee_outputs = false;
    /** When true, always spend all (up to OUTPUT_GROUP_MAX_ENTRIES) or none of the outputs
     * associated with the same address. This helps reduce privacy leaks resulting from address
     * reuse. Dust outputs are not eligible to be added to output groups and thus not considered. */
    bool m_avoid_partial_spends = false;
    /**
     * When true, allow unsafe coins to be selected during Coin Selection. This may spend unconfirmed outputs:
     * 1) Received from other wallets, 2) replacing other txs, 3) that have been replaced.
     */
    bool m_include_unsafe_inputs = false;
    /**
     * When true, skip tx weight check
     */
    bool m_coinstake = false;

    CoinSelectionParams(FastRandomContext& rng_fast, size_t change_output_size, size_t change_spend_size,
                        size_t tx_noinputs_size, bool avoid_partial) :
        rng_fast{rng_fast},
        change_output_size(change_output_size),
        change_spend_size(change_spend_size),
        tx_noinputs_size(tx_noinputs_size),
        m_avoid_partial_spends(avoid_partial)
    {
    }
    CoinSelectionParams(FastRandomContext& rng_fast)
        : rng_fast{rng_fast} {}
};

/** Parameters for filtering which OutputGroups we may use in coin selection.
 * We start by being very selective and requiring multiple confirmations and
 * then get more permissive if we cannot fund the transaction. */
struct CoinEligibilityFilter
{
    /** Minimum number of confirmations for outputs that we sent to ourselves.
     * We may use unconfirmed UTXOs sent from ourselves, e.g. change outputs. */
    const int conf_mine;
    /** Minimum number of confirmations for outputs received from a different wallet. */
    const int conf_theirs;
    /** Maximum number of unconfirmed ancestors aggregated across all UTXOs in an OutputGroup. */
    const uint64_t max_ancestors;
    /** Maximum number of descendants that a single UTXO in the OutputGroup may have. */
    const uint64_t max_descendants;
    /** When avoid_reuse=true and there are full groups (OUTPUT_GROUP_MAX_ENTRIES), whether or not to use any partial groups.*/
    const bool m_include_partial_groups{false};

    CoinEligibilityFilter() = delete;
    CoinEligibilityFilter(int conf_mine, int conf_theirs, uint64_t max_ancestors) : conf_mine(conf_mine), conf_theirs(conf_theirs), max_ancestors(max_ancestors), max_descendants(max_ancestors) {}
    CoinEligibilityFilter(int conf_mine, int conf_theirs, uint64_t max_ancestors, uint64_t max_descendants) : conf_mine(conf_mine), conf_theirs(conf_theirs), max_ancestors(max_ancestors), max_descendants(max_descendants) {}
    CoinEligibilityFilter(int conf_mine, int conf_theirs, uint64_t max_ancestors, uint64_t max_descendants, bool include_partial) : conf_mine(conf_mine), conf_theirs(conf_theirs), max_ancestors(max_ancestors), max_descendants(max_descendants), m_include_partial_groups(include_partial) {}

    bool operator<(const CoinEligibilityFilter& other) const {
        return std::tie(conf_mine, conf_theirs, max_ancestors, max_descendants, m_include_partial_groups)
               < std::tie(other.conf_mine, other.conf_theirs, other.max_ancestors, other.max_descendants, other.m_include_partial_groups);
    }
};

/** A group of UTXOs paid to the same output script. */
struct OutputGroup
{
    /** The list of UTXOs contained in this output group. */
    std::vector<std::shared_ptr<COutput>> m_outputs;
    /** Whether the UTXOs were sent by the wallet to itself. This is relevant because we may want at
     * least a certain number of confirmations on UTXOs received from outside wallets while trusting
     * our own UTXOs more. */
    bool m_from_me{true};
    /** The total value of the UTXOs in sum. */
    CAmount m_value{0};
    /** The minimum number of confirmations the UTXOs in the group have. Unconfirmed is 0. */
    int m_depth{999};
    /** The aggregated count of unconfirmed ancestors of all UTXOs in this
     * group. Not deduplicated and may overestimate when ancestors are shared. */
    size_t m_ancestors{0};
    /** The maximum count of descendants of a single UTXO in this output group. */
    size_t m_descendants{0};
    /** The value of the UTXOs after deducting the cost of spending them at the effective feerate. */
    CAmount effective_value{0};
    /** The fee to spend these UTXOs at the effective feerate. */
    CAmount fee{0};
    /** Indicate that we are subtracting the fee from outputs.
     * When true, the value that is used for coin selection is the UTXO's real value rather than effective value */
    bool m_subtract_fee_outputs{false};
    /** Total weight of the UTXOs in this group. */
    int m_weight{0};

    OutputGroup() {}
    OutputGroup(const CoinSelectionParams& params) :
        m_subtract_fee_outputs(params.m_subtract_fee_outputs)
    {}

    void Insert(const std::shared_ptr<COutput>& output, size_t ancestors, size_t descendants);
    bool EligibleForSpending(const CoinEligibilityFilter& eligibility_filter) const;
    CAmount GetSelectionAmount() const;
};

struct Groups {
    // Stores 'OutputGroup' containing only positive UTXOs (value > 0).
    std::vector<OutputGroup> positive_group;
    // Stores 'OutputGroup' which may contain both positive and negative UTXOs.
    std::vector<OutputGroup> mixed_group;
};

/** Stores several 'Groups' whose were mapped by output type. */
struct OutputGroupTypeMap
{
    // Maps output type to output groups.
    std::map<OutputType, Groups> groups_by_type;
    // All inserted groups, no type distinction.
    Groups all_groups;

    // Based on the insert flag; appends group to the 'mixed_group' and, if value > 0, to the 'positive_group'.
    // This affects both; the groups filtered by type and the overall groups container.
    void Push(const OutputGroup& group, OutputType type, bool insert_positive, bool insert_mixed);
    // Different output types count
    size_t TypesCount() { return groups_by_type.size(); }
};

typedef std::map<CoinEligibilityFilter, OutputGroupTypeMap> FilteredOutputGroups;

/** Compute the waste for this result given the cost of change
 * and the opportunity cost of spending these inputs now vs in the future.
 * If change exists, waste = change_cost + inputs * (effective_feerate - long_term_feerate)
 * If no change, waste = excess + inputs * (effective_feerate - long_term_feerate)
 * where excess = selected_effective_value - target
 * change_cost = effective_feerate * change_output_size + long_term_feerate * change_spend_size
 *
 * Note this function is separate from SelectionResult for the tests.
 *
 * @param[in] inputs The selected inputs
 * @param[in] change_cost The cost of creating change and spending it in the future.
 *                        Only used if there is change, in which case it must be positive.
 *                        Must be 0 if there is no change.
 * @param[in] target The amount targeted by the coin selection algorithm.
 * @param[in] use_effective_value Whether to use the input's effective value (when true) or the real value (when false).
 * @return The waste
 */
[[nodiscard]] CAmount GetSelectionWaste(const std::set<std::shared_ptr<COutput>>& inputs, CAmount change_cost, CAmount target, bool use_effective_value = true);


/** Choose a random change target for each transaction to make it harder to fingerprint the Core
 * wallet based on the change output values of transactions it creates.
 * Change target covers at least change fees and adds a random value on top of it.
 * The random value is between 50ksat and min(2 * payment_value, 1milsat)
 * When payment_value <= 25ksat, the value is just 50ksat.
 *
 * Making change amounts similar to the payment value may help disguise which output(s) are payments
 * are which ones are change. Using double the payment value may increase the number of inputs
 * needed (and thus be more expensive in fees), but breaks analysis techniques which assume the
 * coins selected are just sufficient to cover the payment amount ("unnecessary input" heuristic).
 *
 * @param[in]   payment_value   Average payment value of the transaction output(s).
 * @param[in]   change_fee      Fee for creating a change output.
 */
[[nodiscard]] CAmount GenerateChangeTarget(const CAmount payment_value, const CAmount change_fee, FastRandomContext& rng);

enum class SelectionAlgorithm : uint8_t
{
    BNB = 0,
    KNAPSACK = 1,
    SRD = 2,
    MANUAL = 3,
};

std::string GetAlgorithmName(const SelectionAlgorithm algo);

struct SelectionResult
{
private:
    /** Set of inputs selected by the algorithm to use in the transaction */
    std::set<std::shared_ptr<COutput>> m_selected_inputs;
    /** The target the algorithm selected for. Equal to the recipient amount plus non-input fees */
    CAmount m_target;
    /** The algorithm used to produce this result */
    SelectionAlgorithm m_algo;
    /** Whether the input values for calculations should be the effective value (true) or normal value (false) */
    bool m_use_effective{false};
    /** The computed waste */
    std::optional<CAmount> m_waste;
    /** Total weight of the selected inputs */
    int m_weight{0};

    template<typename T>
    void InsertInputs(const T& inputs)
    {
        // Store sum of combined input sets to check that the results have no shared UTXOs
        const size_t expected_count = m_selected_inputs.size() + inputs.size();
        util::insert(m_selected_inputs, inputs);
        if (m_selected_inputs.size() != expected_count) {
            throw std::runtime_error(STR_INTERNAL_BUG("Shared UTXOs among selection results"));
        }
    }

public:
    explicit SelectionResult(const CAmount target, SelectionAlgorithm algo)
        : m_target(target), m_algo(algo) {}

    SelectionResult() = delete;

    /** Get the sum of the input values */
    [[nodiscard]] CAmount GetSelectedValue() const;

    [[nodiscard]] CAmount GetSelectedEffectiveValue() const;

    void Clear();

    void AddInput(const OutputGroup& group);
    void AddInputs(const std::set<std::shared_ptr<COutput>>& inputs, bool subtract_fee_outputs);

    /** Calculates and stores the waste for this selection via GetSelectionWaste */
    void ComputeAndSetWaste(const CAmount min_viable_change, const CAmount change_cost, const CAmount change_fee);
    [[nodiscard]] CAmount GetWaste() const;

    /**
     * Combines the @param[in] other selection result into 'this' selection result.
     *
     * Important note:
     * There must be no shared 'COutput' among the two selection results being combined.
     */
    void Merge(const SelectionResult& other);

    /** Get m_selected_inputs */
    const std::set<std::shared_ptr<COutput>>& GetInputSet() const;
    /** Get the vector of COutputs that will be used to fill in a CTransaction's vin */
    std::vector<std::shared_ptr<COutput>> GetShuffledInputVector() const;

    bool operator<(SelectionResult other) const;

    /** Get the amount for the change output after paying needed fees.
     *
     * The change amount is not 100% precise due to discrepancies in fee calculation.
     * The final change amount (if any) should be corrected after calculating the final tx fees.
     * When there is a discrepancy, most of the time the final change would be slightly bigger than estimated.
     *
     * Following are the possible factors of discrepancy:
     *  + non-input fees always include segwit flags
     *  + input fee estimation always include segwit stack size
     *  + input fees are rounded individually and not collectively, which leads to small rounding errors
     *  - input counter size is always assumed to be 1vbyte
     *
     * @param[in]  min_viable_change  Minimum amount for change output, if change would be less then we forgo change
     * @param[in]  change_fee         Fees to include change output in the tx
     * @returns Amount for change output, 0 when there is no change.
     *
     */
    CAmount GetChange(const CAmount min_viable_change, const CAmount change_fee) const;

    CAmount GetTarget() const { return m_target; }

    SelectionAlgorithm GetAlgo() const { return m_algo; }

    int GetWeight() const { return m_weight; }
};

std::optional<SelectionResult> SelectCoinsBnB(std::vector<OutputGroup>& utxo_pool, const CAmount& selection_target, const CAmount& cost_of_change);

/** Select coins by Single Random Draw. OutputGroups are selected randomly from the eligible
 * outputs until the target is satisfied
 *
 * @param[in]  utxo_pool    The positive effective value OutputGroups eligible for selection
 * @param[in]  target_value The target value to select for
 * @returns If successful, a SelectionResult, otherwise, std::nullopt
 */
std::optional<SelectionResult> SelectCoinsSRD(const std::vector<OutputGroup>& utxo_pool, CAmount target_value, FastRandomContext& rng);

// Original coin selection algorithm as a fallback
std::optional<SelectionResult> KnapsackSolver(std::vector<OutputGroup>& groups, const CAmount& nTargetValue,
                                              CAmount change_target, FastRandomContext& rng);
} // namespace wallet

#endif // BITCOIN_WALLET_COINSELECTION_H
