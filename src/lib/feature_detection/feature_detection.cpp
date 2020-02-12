#include <map>

#include "MIDAs/MIDAs.h"

#include "feature_detection/feature_detection.hpp"

FeatureDetection::TheoreticalIsotopes normalize_isotopic_distribution(
    std::vector<Isotopic_Distribution> &isotopes, int8_t charge_state,
    double min_perc) {
    auto full_mzs = std::vector<double>(isotopes.size());
    auto probs = std::vector<double>(isotopes.size());
    double max_prob = 0;
    for (size_t i = 0; i < isotopes.size(); ++i) {
        full_mzs[i] = isotopes[i].mw / charge_state;
        probs[i] = isotopes[i].prob;
        if (probs[i] > max_prob) {
            max_prob = probs[i];
        }
    }
    // Normalize the probabilities to 0-1.
    std::vector<double> mzs;
    std::vector<double> perc;
    for (size_t i = 0; i < isotopes.size(); ++i) {
        probs[i] = probs[i] / max_prob;
        if (probs[i] > min_perc) {
            mzs.push_back(full_mzs[i]);
            perc.push_back(probs[i]);
        }
    }
    return {mzs, perc};
}

// FIXME: Remove this.
FeatureDetection::TheoreticalIsotopes
FeatureDetection::theoretical_isotopes_peptide(std::string sequence,
                                               int8_t charge_state,
                                               double min_perc) {
    auto midas = MIDAs(charge_state = charge_state);
    // NOTE: C00: Unmodified cysteine.
    //       C31: Carboxymethylation or Iodoacetic acid.
    //       C32: Carbamidomethylation or Iodoacetamide.
    //       C33: Pyridylethylation.
    midas.Initialize_Elemental_Composition(sequence, "C00", "H", "OH", 1);
    auto isotopes = midas.Coarse_Grained_Isotopic_Distribution();
    return normalize_isotopic_distribution(isotopes, charge_state, min_perc);
}

// FIXME: This is not entirely accurate. The algorithm is affected by the way
// we do the rounding. For example, MS-Isotope seems to round to the closest
// element instead of the lowest, and then adjusts the number of Hydrogens
// up/down, instead of only up.
struct Element {
    std::string name;
    double proportion;
    double mw;
};
static std::vector<Element> averagine = {
    {"C", 4.9384 / 111.1254, 12.011}, {"H", 7.7583 / 111.1254, 1.008},
    {"N", 1.3577 / 111.1254, 14.007}, {"O", 1.4773 / 111.1254, 15.999},
    {"S", 0.0417 / 111.1254, 32.066},
};
// FIXME: Remove this.
FeatureDetection::TheoreticalIsotopes theoretical_isotopes_formula(
    const std::vector<Element> &average_molecular_composition, double mz,
    int8_t charge_state, double min_perc) {
    // Identify the number of whole atoms that we need. Since there is
    // rounding, we need to adjust by adding hydrogen atoms to be
    // at the same mass. Since we are always rounding down, we will only have
    // mass deficit, not surplus.
    std::vector<Element> num_atoms;
    int32_t hydrogen_index = -1;
    double cumulative_mw = 0.0;
    for (size_t i = 0; i < average_molecular_composition.size(); ++i) {
        const auto &atom = average_molecular_composition[i];
        // Round the number of atoms for this element.
        uint64_t rounded_atoms = atom.proportion * mz * charge_state;
        if (rounded_atoms == 0) {
            continue;
        }
        num_atoms.push_back(
            {atom.name, static_cast<double>(rounded_atoms), atom.mw});
        // Calculate the total molecular weight after the rounding.
        // TODO: Should this lookup be done on a hash table instead of carrying
        // it around in the Element object?
        cumulative_mw += rounded_atoms * atom.mw;
        // Find the index for Hydrogen if we have it.
        if (atom.name == "H") {
            hydrogen_index = i;
        }
    }
    // Calculate the difference between the molecular weight after rounding and
    // the expected mass. The number is calculated in whole units, as this is
    // the number of Hydrogens we are going to add to our table.
    uint64_t extra_hydrogens = mz * charge_state - cumulative_mw;
    if (hydrogen_index == -1) {
        num_atoms.push_back({"H", 0.0, 1.008});
        hydrogen_index = num_atoms.size() - 1;
    }
    num_atoms[hydrogen_index].proportion += extra_hydrogens;

    // Build the chemical formula for the given mz and charge_state.
    std::string formula = "";
    for (const auto &atom : num_atoms) {
        uint64_t num_atoms = atom.proportion;
        formula += atom.name + std::to_string(num_atoms);
    }
    auto midas = MIDAs(charge_state = charge_state);
    auto isotopes = midas.Coarse_Grained_Isotopic_Distribution();
    return normalize_isotopic_distribution(isotopes, charge_state, min_perc);
}

// FIXME: Remove this.
std::optional<FeatureDetection::Feature> FeatureDetection::build_feature(
    const std::vector<bool> &peaks_in_use,
    const std::vector<Centroid::Peak> &peaks,
    const std::vector<Search::KeySort<double>> &peaks_rt_key,
    const TheoreticalIsotopes &theoretical_isotopes, double tolerance_rt,
    double retention_time, double discrepancy_threshold, int8_t charge_state) {
    const auto &mzs = theoretical_isotopes.mzs;
    const auto &perc = theoretical_isotopes.percs;
    // Basic sanitation.
    if (mzs.size() != perc.size() || mzs.size() == 0) {
        return std::nullopt;
    }

    // Find the peaks in range for matching.
    double min_rt = retention_time - tolerance_rt;
    double max_rt = retention_time + tolerance_rt;
    size_t min_j = Search::lower_bound(peaks_rt_key, min_rt);
    size_t max_j = peaks_rt_key.size();
    std::vector<Centroid::Peak> peaks_in_range;
    for (size_t j = min_j; j < max_j; ++j) {
        if (peaks_rt_key[j].sorting_key > max_rt) {
            break;
        }
        auto &peak = peaks[peaks_rt_key[j].index];
        if (peaks_in_use[peak.id]) {
            continue;
        }
        if ((peak.fitted_mz + peak.fitted_sigma_mz * 2) < mzs[0] ||
            (peak.fitted_mz - peak.fitted_sigma_mz * 2) > mzs[mzs.size() - 1]) {
            continue;
        }
        peaks_in_range.push_back(peak);
    }
    if (peaks_in_range.empty()) {
        return std::nullopt;
    }
    // Sort the peaks in range by mz for a faster search.
    std::sort(peaks_in_range.begin(), peaks_in_range.end(),
              [](const Centroid::Peak &p1, const Centroid::Peak &p2) {
                  return (p1.fitted_mz < p2.fitted_mz);
              });

    // Find the reference node and the list of candidates for each node.
    size_t reference_node_index = 0;
    bool found_reference_node = false;
    std::vector<std::vector<const Centroid::Peak *>> candidate_list;
    for (size_t k = 0; k < mzs.size(); ++k) {
        if (perc[k] == 1) {
            reference_node_index = k;
            found_reference_node = true;
        }
        double theoretical_mz = mzs[k];
        std::vector<const Centroid::Peak *> candidates_node;
        for (const auto &peak : peaks_in_range) {
            if ((peak.fitted_mz + peak.fitted_sigma_mz) > theoretical_mz &&
                (peak.fitted_mz - peak.fitted_sigma_mz) < theoretical_mz) {
                candidates_node.push_back(&peak);
            }
        }
        candidate_list.push_back(candidates_node);
    }
    if (!found_reference_node || candidate_list.empty()) {
        return std::nullopt;
    }

    // In case more than one peak is linked to the reference mz isotope,
    // the sequence with the less matching error should be selected. In
    // order to do so, the heights for each candidate must be normalized
    // by the reference isotope height.
    std::vector<const Centroid::Peak *> selected_candidates;
    std::vector<double> selected_candidates_norm_height;
    double min_total_distance = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < candidate_list[reference_node_index].size(); ++i) {
        const auto ref_candidate = candidate_list[reference_node_index][i];
        std::vector<const Centroid::Peak *> selected_candidates_for_reference;
        std::vector<double> selected_candidates_for_reference_norm_height;
        double total_distance = 0.0;
        // We need to do a forwards and backwards pass in order for
        // the algorithm to work, since we are be stopping the pattern matching
        // once we find a peak where the minimum height difference is greater
        // than the given discrepancy_threshold.
        //     reference_node_index->0           // Backwards
        //     reference_node_index->mzs.size()  // Forward
        // The peaks are reversed after the backwards pass, so that we keep the
        // proper ascending mz ordering.
        auto select_candidates = [&](size_t k) -> bool {
            if (k == reference_node_index) {
                selected_candidates_for_reference.push_back(ref_candidate);
                selected_candidates_for_reference_norm_height.push_back(1.0);
                return true;
            }

            // Find the best matching candidate for the selected reference. We
            // define here the best matching candidate as the candidate peak
            // with the minimum euclidean distance from the expected
            // theoretical peak.
            double theoretical_mz = mzs[k];
            double theoretical_percentage = perc[k];
            double min_distance = std::numeric_limits<double>::infinity();
            double selected_normalized_height = 0.0;
            const Centroid::Peak *selected_candidate;
            double selected_normalized_height_diff = 0.0;
            for (size_t j = 0; j < candidate_list[k].size(); ++j) {
                const auto candidate = candidate_list[k][j];
                double normalized_height =
                    candidate->fitted_height / ref_candidate->fitted_height;
                double a = candidate->fitted_mz - theoretical_mz;
                double b = candidate->fitted_rt - retention_time;
                double c = normalized_height - theoretical_percentage;
                double distance = std::sqrt(a * a + b * b + c * c);
                if (distance < min_distance) {
                    min_distance = distance;
                    selected_normalized_height = normalized_height;
                    selected_candidate = candidate;
                    selected_normalized_height_diff = std::abs(c);
                }
            }

            if (selected_normalized_height_diff > discrepancy_threshold ||
                selected_normalized_height == 0.0) {
                return false;
            }
            selected_candidates_for_reference.push_back(selected_candidate);
            selected_candidates_for_reference_norm_height.push_back(
                selected_normalized_height);
            total_distance += min_distance;
            return true;
        };
        // Backwards from reference_node_index.
        for (size_t k = reference_node_index; k > 0; --k) {
            int success = select_candidates(k);
            if (success) {
                continue;
            }
            if (!success) {
                break;
            }
        }
        // Candidate at exactly 0.
        select_candidates(0);
        // Reverse list of candidates.
        std::reverse(selected_candidates_for_reference.begin(),
                     selected_candidates_for_reference.end());
        std::reverse(selected_candidates_for_reference_norm_height.begin(),
                     selected_candidates_for_reference_norm_height.end());
        // Forward from reference_node_index.
        for (size_t k = reference_node_index + 1; k < mzs.size(); ++k) {
            int success = select_candidates(k);
            if (success) {
                continue;
            }
            if (!success) {
                break;
            }
        }
        if (total_distance < min_total_distance) {
            selected_candidates = selected_candidates_for_reference;
            selected_candidates_norm_height =
                selected_candidates_for_reference_norm_height;
            min_total_distance = total_distance;
        }
    }
    if (selected_candidates.empty()) {
        return std::nullopt;
    }

    // Build the actual feature data.
    Feature feature = {};
    // FIXME: Currently assuming that the minimum detected isotope is the
    // monoisotopic peak, but THIS MIGHT NOT BE THE CASE. For simplicity and to
    // keep the flow going I'll leave this for now, but must go back and FIX
    // it.
    feature.monoisotopic_mz = selected_candidates[0]->fitted_mz;
    feature.monoisotopic_height = selected_candidates[0]->fitted_height;
    // Find the weighted average mz and the average height of the selected
    // isotopes.
    feature.average_mz = 0.0;
    feature.total_height = 0.0;
    feature.average_rt = 0.0;
    feature.average_rt_delta = 0.0;
    feature.average_rt_sigma = 0.0;
    feature.average_mz_sigma = 0.0;
    for (size_t i = 0; i < selected_candidates.size(); ++i) {
        auto candidate = selected_candidates[i];
        feature.total_height += candidate->fitted_height;
        feature.average_mz += candidate->fitted_height * candidate->fitted_mz;
        feature.average_rt += candidate->fitted_rt;
        feature.average_rt_delta += candidate->rt_delta;
        feature.average_rt_sigma += candidate->fitted_sigma_rt;
        feature.average_mz_sigma += candidate->fitted_sigma_mz;
        feature.peak_ids.push_back(candidate->id);
    }
    if (feature.total_height == 0) {
        return std::nullopt;
    }
    feature.average_mz /= feature.total_height;
    feature.average_rt /= selected_candidates.size();
    feature.average_rt_delta /= selected_candidates.size();
    feature.average_rt_sigma /= selected_candidates.size();
    feature.average_mz_sigma /= selected_candidates.size();
    feature.charge_state = charge_state;
    return feature;
}

// FIXME: Remove this.
std::vector<FeatureDetection::Feature> FeatureDetection::feature_detection(
    const std::vector<Centroid::Peak> &peaks,
    const RawData::RawData &raw_data_ms2,
    const IdentData::IdentData &ident_data,
    const std::vector<Link::LinkedMsms> &link_table_msms,
    const std::vector<Link::LinkedMsms> &link_table_idents,
    double discrepancy_threshold) {
    std::vector<Feature> features;

    // The proposed algorithm goes as follows:
    // 0.- Copy and sort the necessary vectors for the use of binary search
    //     (link_table_idents is sorted by msms, as that is the key being
    //     used for searching). The peaks array and link_table_msms array
    //     should have already been sorted by peak_id, which is what we
    //     want, as we are going to start matching peaks from highest
    //     intensity to lowest.
    // 1.- For each linked peak on the link_table_msms, find it's associated
    //     entry on the link_table_idents.
    // 2.- If the entry is found, use it to generate a theoretical isotopic
    //     distribution, otherwise, averagine will be generated.
    // 3.- We try to find the proposed peaks from the theoretical
    //     distribution on the peaks array (Maximum likelihood).
    // 4.- The peaks are marked as non available for future use. This is
    //     a greedy algorithm.

    // Copy and sort key vectors.
    auto idents_msms_key =
        std::vector<Search::KeySort<uint64_t>>(link_table_idents.size());
    for (size_t i = 0; i < link_table_idents.size(); ++i) {
        // idents_msms_key[i] = {i, link_table_idents[i].msms_id};
    }
    {
        auto sorting_key_func = [](const Search::KeySort<uint64_t> &p1,
                                   const Search::KeySort<uint64_t> &p2) {
            return (p1.sorting_key < p2.sorting_key);
        };
        std::sort(idents_msms_key.begin(), idents_msms_key.end(),
                  sorting_key_func);
    }
    auto peaks_rt_key = std::vector<Search::KeySort<double>>(peaks.size());
    for (size_t i = 0; i < peaks.size(); ++i) {
        peaks_rt_key[i] = {i, peaks[i].fitted_rt};
    }
    {
        auto sorting_key_func = [](const Search::KeySort<double> &p1,
                                   const Search::KeySort<double> &p2) {
            return (p1.sorting_key < p2.sorting_key);
        };
        std::sort(peaks_rt_key.begin(), peaks_rt_key.end(), sorting_key_func);
    }
    // We use this variable to keep track of the peaks we have already linked.
    auto peaks_in_use = std::vector<bool>(peaks.size());

    // TODO: We should probably prioritize the MSMS events that HAVE an
    // identification, instead of being intensity based only.
    // TODO: We are linking msms events independently, but we know that
    // multiple msms events can be linked to any given peak. If the selected
    // identification is the same there is no problem, and if there is
    // a majority of identifications they should result in a consensus.
    // However, if the identification is ambiguous, we can choose to use
    // averagine instead for that peak. To avoid duplication of effor, we
    // should resolve conflicts and perform feature matching based only on the
    // list of consensus candidates.
    for (const auto &linked_msms : link_table_msms) {
        // Find lower bound on the idents_msms_key.
        auto i = lower_bound(idents_msms_key, linked_msms.msms_id);
        auto charge_state = raw_data_ms2.scans[linked_msms.scan_index]
                                .precursor_information.charge;

        // We need to ensure that the search succeeded. For example if we are
        // trying to find a number on an empty array, the lower bound will be 0,
        // but that doesn't ensure that we found the right element. A similar
        // situation applies if we try to find a needle not contained on the
        // haystack, in which case the search will return the closest lower
        // bound on the search array or last index of the array.
        auto index = idents_msms_key[i].index;
        auto ident = link_table_idents[index];
        const auto &peak = peaks[linked_msms.entity_id];
        TheoreticalIsotopes theoretical_isotopes = {};
        if (linked_msms.msms_id != ident.msms_id) {
            // Generate a theoretical_isotope_distribution based on averagine.
            // FIXME: This needs to be checked. For now we are ignoring the
            // msms events without identifications to get initial measurements
            // with our data.
            // theoretical_isotopes = theoretical_isotopes_formula(
            // averagine, peak.fitted_mz, charge_state, 0.01);
        } else {
            // Generate a theoretical_isotope_distribution based on the
            // given sequence.
            // FIXME: This algorithm WILL be affected by modifications. If we
            // don't consider them, the m/z results will not match the peaks in
            // our peak list.
            auto sequence = ident_data.spectrum_ids[ident.entity_id].sequence;
            theoretical_isotopes =
                FeatureDetection::theoretical_isotopes_peptide(
                    sequence, charge_state, 0.01);
        }
        if (theoretical_isotopes.mzs.empty() ||
            theoretical_isotopes.percs.empty()) {
            continue;
        }

        // We use the retention time of the APEX of the matched peak,
        // not the msms event.
        double peak_rt = peak.fitted_rt;
        double peak_rt_sigma = peak.fitted_sigma_rt;
        auto maybe_feature = build_feature(
            peaks_in_use, peaks, peaks_rt_key, theoretical_isotopes,
            peak_rt_sigma * 2, peak_rt, discrepancy_threshold, charge_state);
        if (maybe_feature) {
            auto feature = maybe_feature.value();
            // feature.msms_id = linked_msms.msms_id;
            features.push_back(feature);
            // Remove used peaks on the feature from the pool.
            for (const auto &peak_id : feature.peak_ids) {
                peaks_in_use[peak_id] = true;
            }
        }
    }

    // Sort features by height and assign feature ids.
    std::sort(features.begin(), features.end(), [](auto &a, auto &b) {
        return (a.total_height > b.total_height);
    });
    for (size_t i = 0; i < features.size(); ++i) {
        features[i].id = i;
    }

    return features;
}

std::vector<std::vector<uint64_t>> find_all_paths(
    FeatureDetection::CandidateGraph &graph, uint64_t root_node) {
    std::vector<std::vector<uint64_t>> paths;
    std::vector<std::vector<uint64_t>> stack;
    stack.push_back({root_node});
    while (!stack.empty()) {
        auto curr_path = stack.back();
        stack.pop_back();
        auto last = curr_path[curr_path.size() - 1];
        auto &root_node = graph[last];
        if (root_node.visited) {
            continue;
        }

        bool path_finished = true;
        for (const auto &node : root_node.nodes) {
            if (graph[node].visited) {
                continue;
            }
            path_finished = false;
            auto new_path = curr_path;
            new_path.push_back(node);
            stack.push_back(new_path);
        }
        if (path_finished) {
            paths.push_back(curr_path);
        }
    }

    return paths;
}

// NOTE: The order matters. A should be the path we are exploring, and B the
// reference theoretical path.
struct OptimalPath {
    double dot;
    size_t min_i;
    size_t max_i;
};
OptimalPath rolling_weighted_cosine_sim(std::vector<double> &A,
                                        std::vector<double> &B) {
    // We need at least 2 isotopes to form a feature.
    if (A.size() < 2 || B.size() < 2) {
        return {0.0, 0, 0};
    }
    // Find the maximum b position and precalculate the norm of B.
    double norm_b = 0.0;
    double max_b = 0.0;
    size_t max_b_index = 0;
    for (size_t i = 0; i < B.size(); ++i) {
        norm_b += B[i] * B[i] * B[i];
        if (B[i] > max_b) {
            max_b = B[i];
            max_b_index = i;
        }
    }
    norm_b = std::sqrt(norm_b);

    // We are going to roll the maximum of B (100%) through each position of A,
    // which means we are to perform k == A.size() cycles.
    double best_dot = 0.0;
    size_t best_path_min_i = 0;
    size_t best_path_max_i = 0;
    for (size_t k = 0; k < A.size(); ++k) {
        size_t min_i = k < max_b_index ? 0 : k - max_b_index;
        size_t max_i = std::min(A.size(), B.size() - max_b_index + k);
        size_t min_j = k < max_b_index ? max_b_index - k : 0;
        size_t n_iter = max_i - min_i;

        double norm_a = 0.0;
        double dot = 0.0;
        // Center->Left.
        size_t true_min_i = min_i;
        size_t true_max_i = max_i;
        for (size_t i = 1; i <= k; ++i) {
            double a = A[min_i + k - i];
            double b = B[min_j + k - i];

            // Check if difference between theoretical and measured is too big.
            double normalized_a = a / A[k];
            double abs_diff = std::abs(normalized_a - b / 100.0);
            if (abs_diff >= 0.35) {
                true_min_i = min_i + k - i + 1;
                break;
            }

            norm_a += a * a * b;
            dot += a * b * b;
        }
        // Center->Right.
        for (size_t i = k; i < min_i + n_iter; ++i) {
            double a = A[min_i + i];
            double b = B[min_j + i];

            // Check if difference between theoretical and measured is too big.
            double normalized_a = a / A[k];
            double abs_diff = std::abs(normalized_a - b / 100.0);
            if (abs_diff >= 0.35) {
                true_max_i = i;
                break;
            }

            norm_a += a * a * b;
            dot += a * b * b;
        }
        norm_a = std::sqrt(norm_a);
        dot = dot / (norm_a * norm_b);
        if (dot > best_dot && true_max_i > true_min_i &&
            (true_max_i - true_min_i) > 1) {
            best_dot = dot;
            best_path_min_i = true_min_i;
            best_path_max_i = true_max_i;
        }
    }
    size_t best_path_size = best_path_max_i - best_path_min_i;
    if (best_path_size <= 1) {
        return {0.0, 0, 0};
    }
    return {best_dot, best_path_min_i, best_path_max_i};
}

std::map<double, std::vector<double>> averagine_table = {
    {100.226, {100, 4.79, 0.28}},
    {110.221, {100, 5.89, 0.4}},
    {120.172, {100, 6.61, 0.63, 0.03}},
    {130.381, {100, 6.41, 0.38, 0.03}},
    {140.376, {100, 7.86, 0.42, 0.02}},
    {150.456, {100, 7.19, 0.27, 0.02}},
    {160.217, {100, 8.74, 0.76, 0.05}},
    {170.212, {100, 9.23, 0.8}},
    {180.292, {100, 10.2, 0.97, 0.02}},
    {190.243, {100, 9.7, 1.1, 0.04}},
    {200.238, {100, 10.89, 0.99, 0.09}},
    {210.447, {100, 11.13, 0.83, 0.04}},
    {220.527, {100, 10.82, 1.13, 0.08}},
    {230.288, {100, 12.12, 1.17, 0.11}},
    {240.283, {100, 13.72, 1.45, 0.1}},
    {250.363, {100, 13.75, 1.39, 0.1}},
    {260.358, {100, 13.91, 1.43, 0.17}},
    {270.309, {100, 14.08, 2, 0.08, 0.02}},
    {280.389, {100, 13.99, 1.93, 0.2}},
    {290.598, {100, 14.35, 1.79, 0.13}},
    {300.359, {100, 16.65, 2.12, 0.17}},
    {310.354, {100, 16.94, 2.16, 0.16, 0.02}},
    {320.434, {100, 16.49, 2.1, 0.25, 0.04}},
    {330.429, {100, 18.62, 2.36, 0.16, 0.02}},
    {340.38, {100, 18.49, 2.67, 0.38}},
    {350.589, {100, 17.48, 2.52, 0.2}},
    {360.455, {100, 19.64, 3.42, 0.22}},
    {370.43, {100, 19.51, 2.79, 0.25, 0.02}},
    {380.615, {100, 19.6, 2.97, 0.22, 0.04}},
    {390.505, {100, 20.41, 3.26, 0.29, 0.02}},
    {400.5, {100, 22.29, 2.97, 0.45}},
    {410.58, {100, 21.66, 3.11, 0.41, 0.03}},
    {420.66, {100, 20.75, 3.34, 0.4, 0.04}},
    {430.526, {100, 23.81, 3.69, 0.56, 0.06}},
    {440.521, {100, 24.09, 3.93, 0.45, 0.08}},
    {450.601, {100, 24.57, 3.95, 0.8, 0.05}},
    {459.568, {100, 24.02, 4.23, 0.61, 0.08}},
    {469.563, {100, 25.73, 4.1, 0.63, 0.08}},
    {479.643, {100, 25.73, 4.3, 0.69, 0.07}},
    {489.828, {100, 26.27, 4.33, 0.68, 0.05}},
    {499.589, {100, 27.35, 5.17, 0.72, 0.09}},
    {509.584, {100, 28.48, 5.02, 0.94, 0.09}},
    {519.664, {100, 28.31, 5.53, 0.67, 0.04}},
    {529.659, {100, 27.65, 5.41, 0.9, 0.07}},
    {539.634, {100, 28.84, 5.69, 0.87, 0.04}},
    {549.714, {100, 27.9, 5.51, 0.93, 0.13}},
    {559.709, {100, 31.45, 5.66, 0.72, 0.08, 0.03}},
    {569.66, {100, 31.57, 6.33, 0.83, 0.08}},
    {579.655, {100, 31.46, 6.3, 0.86, 0.14}},
    {589.735, {100, 31.03, 6.6, 1.14, 0.21, 0.03}},
    {599.73, {100, 32.68, 6.4, 1.18, 0.13}},
    {609.81, {100, 33.94, 7.26, 1.07, 0.07, 0.03}},
    {619.7, {100, 35.01, 6.55, 1.01, 0.16}},
    {629.78, {100, 32.71, 7.22, 1.24, 0.11}},
    {639.731, {100, 33.9, 7.17, 1.17, 0.2}},
    {649.726, {100, 36.01, 8.15, 1.22, 0.04, 0.06}},
    {659.806, {100, 34.84, 8.01, 1.12, 0.1}},
    {669.801, {100, 35.71, 8.1, 1.59, 0.22}},
    {679.881, {100, 37.98, 8.78, 1.65, 0.16, 0.03}},
    {689.876, {100, 37.31, 8.88, 1.45, 0.16}},
    {699.851, {100, 38.03, 8.92, 1.5, 0.24, 0.03}},
    {709.846, {100, 38.45, 9.23, 1.45, 0.21, 0.03}},
    {720.116, {100, 38.47, 8.84, 1.55, 0.18, 0.04}},
    {729.877, {100, 38.27, 9.41, 1.71, 0.19, 0.03}},
    {739.872, {100, 40.52, 10.44, 1.83, 0.11}},
    {749.952, {100, 39.01, 10.02, 1.8, 0.27, 0.05}},
    {759.947, {100, 40.33, 9.87, 2.12, 0.23, 0.03}},
    {770.027, {100, 40.15, 10.01, 1.92, 0.44, 0.02, 0.02}},
    {779.917, {100, 40.56, 10.63, 1.82, 0.29, 0.03}},
    {790.102, {100, 41.56, 10.64, 2.06, 0.31, 0.05}},
    {800.182, {100, 42.16, 10.52, 2, 0.25, 0.02}},
    {809.943, {100, 43.26, 11.45, 1.98, 0.25, 0.09}},
    {820.023, {100, 43.67, 11.94, 2.31, 0.33, 0.08}},
    {830.018, {100, 46.25, 12.02, 2.73, 0.44, 0.06}},
    {840.098, {100, 43.45, 11.81, 2.19, 0.32, 0.11, 0.02}},
    {850.093, {100, 45.3, 12.16, 2.47, 0.47, 0.02}},
    {860.068, {100, 45.09, 11.92, 2.63, 0.48, 0.06}},
    {870.253, {100, 46.01, 12.03, 2.32, 0.42, 0.05}},
    {880.014, {100, 46.7, 13.43, 3.13, 0.49, 0.05}},
    {890.009, {100, 50.13, 14.52, 3.29, 0.47, 0.07, 0.02}},
    {900.089, {100, 50.21, 13.51, 3, 0.5, 0.13, 0.02}},
    {910.169, {100, 48.78, 13.69, 2.75, 0.55, 0.13, 0.02}},
    {920.164, {100, 49.35, 14.77, 3.16, 0.54, 0.05}},
    {930.244, {100, 49.34, 14.98, 3.18, 0.44, 0.13, 0.05}},
    {940.239, {100, 51.35, 14.48, 3.12, 0.47, 0.08, 0.02}},
    {950.085, {100, 52.25, 14.81, 3.25, 0.67, 0.03, 0.02}},
    {960.08, {100, 51.19, 15.13, 3.13, 0.68, 0.07, 0.03}},
    {970.16, {100, 52.72, 16.24, 3.84, 0.71, 0.12, 0.02}},
    {980.155, {100, 51.74, 16.58, 3.75, 0.71, 0.14}},
    {990.235, {100, 52.21, 15.76, 3.55, 0.65, 0.09, 0.03}},
    {1000.315, {100, 51.86, 16.47, 3.96, 0.57, 0.12}},
    {1010.31, {100, 53.51, 17.41, 3.64, 0.98, 0.14, 0.04}},
    {1020.261, {100, 52.04, 17.87, 4.08, 0.87, 0.1, 0.02}},
    {1030.47, {100, 53.59, 17.21, 4.77, 0.86, 0.09}},
    {1040.231, {100, 55.44, 17.27, 4.44, 0.86, 0.2, 0.02}},
    {1050.226, {100, 56.67, 17.81, 5.11, 0.65, 0.14, 0.02, 0.02}},
    {1060.306, {100, 56.31, 19.53, 4.51, 1.16, 0.18, 0.02}},
    {1070.301, {100, 57.68, 19.65, 4.52, 0.93, 0.18, 0.02}},
    {1080.381, {100, 58.75, 19.44, 4.46, 0.83, 0.11}},
    {1090.461, {100, 58.25, 20.02, 4.94, 1.01, 0.2, 0.06, 0.02}},
    {1100.327, {100, 60.46, 20.57, 5.17, 0.9, 0.24, 0.02}},
    {1110.302, {100, 59.45, 20.84, 4.6, 1.3, 0.21, 0.02}},
    {1120.297, {100, 59.86, 21.03, 5.21, 1.09, 0.11}},
    {1130.377, {100, 63.96, 21.84, 5.59, 1.06, 0.27, 0.04}},
    {1140.372, {100, 62.02, 21.26, 5.58, 1.2, 0.27, 0.02, 0.02}},
    {1150.452, {100, 63.67, 22.67, 5.57, 1.12, 0.14, 0.04, 0.02, 0.02}},
    {1160.447, {100, 62.82, 22.47, 6.27, 1.22, 0.19, 0.06, 0.02}},
    {1170.398, {100, 62.6, 23.42, 6.47, 1.42, 0.37, 0.04, 0.04}},
    {1180.478, {100, 65.15, 22.33, 6.58, 1.59, 0.2, 0.02, 0.02}},
    {1190.368, {100, 69.24, 25, 6.69, 1.46, 0.34, 0.02}},
    {1200.448, {100, 64.89, 23.75, 6.61, 1.56, 0.36, 0.06, 0.02}},
    {1210.443, {100, 65.84, 24.64, 6.81, 1.61, 0.26, 0.02, 0.02}},
    {1220.523, {100, 65.66, 24.76, 6.78, 1.35, 0.3}},
    {1230.518, {100, 68.02, 25.08, 6.66, 1.11, 0.32, 0.06}},
    {1240.598, {100, 65.66, 25.39, 7.23, 1.7, 0.32, 0.04, 0.02}},
    {1250.464, {100, 67.29, 25.12, 7.01, 1.71, 0.24, 0.02, 0.02}},
    {1260.544, {100, 68.54, 26.48, 6.94, 1.69, 0.35, 0.04}},
    {1270.519, {100, 69.28, 27.14, 7.81, 1.96, 0.39, 0.1}},
    {1280.514, {100, 67.8, 26.48, 8.12, 2.24, 0.39, 0.04, 0.02}},
    {1290.594, {100, 65.39, 25.83, 6.81, 1.8, 0.26, 0.1, 0.02}},
    {1300.589, {100, 68.98, 27.61, 8.34, 1.61, 0.35, 0.06}},
    {1310.669, {100, 70.44, 27.46, 8.31, 1.75, 0.17, 0.04}},
    {1320.535, {100, 72.07, 30.1, 7.69, 2.23, 0.28, 0.04}},
    {1330.615, {100, 73.65, 28.86, 8.35, 2.28, 0.32, 0.02}},
    {1340.824, {100, 73.28, 29, 8.02, 2.5, 0.34, 0.11, 0.02}},
    {1350.904, {100, 70.91, 28.35, 8.96, 1.75, 0.32, 0.02}},
    {1359.762, {100, 72.36, 28.83, 9.05, 2.04, 0.55, 0.09, 0.02}},
    {1369.842, {100, 73.29, 30.55, 7.81, 2.25, 0.56, 0.09}},
    {1379.837, {100, 73.77, 32.27, 8.8, 2.44, 0.5, 0.09}},
    {1389.917, {100, 75.99, 30.6, 8.42, 2.53, 0.52, 0.04}},
    {1399.807, {100, 74.06, 31.98, 9.47, 2.12, 0.46, 0.07}},
    {1409.887, {100, 78.89, 32.64, 9.85, 2.4, 0.43, 0.11}},
    {1419.882, {100, 78.06, 31.3, 9.99, 2.31, 0.71, 0.04, 0.02, 0.02}},
    {1429.833, {100, 77.67, 32.39, 10.39, 2.53, 0.42, 0.07, 0.02, 0.02}},
    {1439.913, {100, 75.87, 33.66, 10.62, 2.57, 0.42, 0.04, 0.02}},
    {1449.908, {100, 75.48, 31.87, 10.79, 2.59, 0.6, 0.11, 0.04}},
    {1459.988, {100, 75.55, 33.67, 9.89, 2.81, 0.62, 0.09, 0.04}},
    {1469.983, {100, 77.62, 33.47, 10.37, 2.68, 0.61, 0.16, 0.02}},
    {1479.958, {100, 80.43, 35.76, 11.2, 2.96, 0.49, 0.16}},
    {1489.953, {100, 77.82, 34.71, 10.35, 2.79, 0.84, 0.05}},
    {1500.033, {100, 79.92, 36.45, 11.37, 2.66, 0.62, 0.09}},
    {1509.899, {100, 80.97, 35.86, 11.74, 2.78, 0.56, 0.16}},
    {1519.979, {100, 80.83, 37.36, 11.74, 3.06, 0.61, 0.19, 0.02}},
    {1530.059, {100, 79.36, 36.89, 11.8, 2.74, 0.83, 0.21, 0.02}},
    {1540.054, {100, 84.88, 38.88, 13.05, 3.38, 0.84, 0.17}},
    {1550.134, {100, 84.66, 37.89, 12.42, 3.37, 0.81, 0.1, 0.07, 0.02}},
    {1560.024, {100, 83.99, 39.94, 11.95, 3.44, 0.79, 0.31, 0.02}},
    {1570.104, {100, 84.54, 37.79, 12.35, 3.45, 0.96, 0.22, 0.02, 0.02}},
    {1579.97, {100, 83.72, 39.88, 13.26, 3.7, 0.65, 0.29, 0.05}},
    {1590.05, {100, 87.26, 40.71, 15.57, 3.35, 1.12, 0.17, 0.07}},
    {1600.045, {100, 82.95, 40.86, 13.8, 3.63, 0.78, 0.15, 0.02}},
    {1610.125, {100, 85.95, 41.33, 14.19, 3.75, 0.96, 0.25}},
    {1620.205, {100, 86.9, 40.56, 13.99, 4.2, 1.29, 0.22}},
    {1630.2, {100, 85.74, 40.87, 14.73, 4.27, 0.79, 0.25, 0.02}},
    {1640.175, {100, 86.5, 41.06, 13.99, 4.15, 0.76, 0.22, 0.02, 0.02}},
    {1650.17, {100, 93.01, 45.28, 16.48, 4.85, 0.91, 0.23, 0.03, 0.03}},
    {1660.121, {100, 87.73, 43.32, 15.17, 4.51, 1.11, 0.15, 0.03}},
    {1670.116, {100, 92.29, 45.12, 16.47, 4.57, 1.12, 0.16, 0.03}},
    {1680.196, {100, 89.65, 44.38, 16.31, 4.24, 0.82, 0.23}},
    {1690.191, {100, 91.13, 44.36, 15.11, 3.68, 1.28, 0.1, 0.03}},
    {1700.271, {100, 89.79, 46.3, 16.44, 4.52, 1.27, 0.13, 0.05, 0.03}},
    {1710.351, {100, 92.84, 46.64, 17.43, 4.66, 1.11, 0.42, 0.13}},
    {1720.241, {100, 91.48, 47.07, 17.65, 5.34, 1.26, 0.18, 0.08, 0.03}},
    {1730.321, {100, 91.41, 46.12, 16.99, 5.09, 1.12, 0.21, 0.08}},
    {1740.187, {100, 93.9, 49.31, 18.31, 5.54, 1.53, 0.19, 0.11}},
    {1750.267, {100, 91.7, 48.96, 16.07, 5.3, 1.26, 0.16, 0.05}},
    {1760.262, {100, 97.9, 49.85, 18.03, 4.77, 1.31, 0.44, 0.11}},
    {1770.342, {100, 94.63, 49.92, 19.3, 5.39, 1.46, 0.27, 0.03}},
    {1780.337, {100, 96.91, 49.7, 19.85, 6.08, 1.15, 0.14}},
    {1790.417, {100, 96.24, 49.71, 18.96, 5.67, 1.31, 0.49, 0.03}},
    {1800.497, {100, 92.46, 49.83, 18.48, 5.66, 1.21, 0.43, 0.11}},
    {1810.258, {100, 98.61, 51.38, 20.31, 5.5, 1.64, 0.25, 0.17}},
    {1820.338, {100, 97.07, 49.67, 17.93, 7.01, 1.51, 0.38, 0.11}},
    {1830.143, {100, 98.48, 58.05, 24.58, 7.46, 2.22, 0.52, 0.2, 0.03}},
    {1840.138, {100, 97.94, 59.77, 24.48, 8.8, 2.38, 0.62, 0.21, 0.09}},
    {1850.408, {100, 99.86, 55.1, 22.07, 6.57, 1.48, 0.4}},
    {1860.488, {98.76, 100, 54.16, 19.5, 6.88, 2, 0.48}},
    {1870.483, {98.13, 100, 56.47, 20.83, 5.87, 1.67, 0.37, 0.03}},
    {1880.563, {98.6, 100, 52.6, 20.61, 6.49, 1.91, 0.59, 0.03}},
    {1890.409, {100, 97.06, 55.44, 21.46, 6.47, 1.19, 0.51, 0.25, 0.03}},
    {1900.214, {98.5, 100, 61.44, 27.21, 9.67, 2.73, 0.51, 0.15, 0.03, 0.06}},
    {1910.209, {96.21, 100, 60.96, 29.52, 9.65, 3.55, 0.48, 0.21}},
    {1920.479, {98.35, 100, 54.26, 22.26, 7.54, 1.91, 0.23, 0.06, 0.06}},
    {1930.559, {96.7, 100, 57.96, 23.19, 7.13, 1.78, 0.49, 0.06, 0.03, 0.03}},
    {1940.554, {93.88, 100, 56.66, 21.78, 7.42, 1.78, 0.56, 0.06, 0.03}},
    {1950.634, {96.36, 100, 57, 22.85, 7.59, 1.8, 0.63, 0.06}},
    {1960.5, {95.08, 100, 60.73, 23.35, 7.61, 2.03, 0.43, 0.12}},
    {1970.475, {91.77, 100, 55.96, 23.6, 7.87, 2.03, 0.56, 0.11, 0.03}},
    {1980.28, {91.42, 100, 59.32, 29.7, 11.51, 2.96, 0.83, 0.12}},
    {1990.36, {90.43, 100, 63.33, 29.55, 10.73, 3.16, 0.83, 0.12}},
    {2000.63, {93.79, 100, 58, 25.06, 8.2, 1.84, 0.66, 0.06, 0.09}},
    {2010.625, {90.13, 100, 56.65, 23.53, 8.15, 2.05, 0.51, 0.11}},
    {2020.705, {89.14, 100, 59.48, 24.67, 8.33, 2.02, 0.31, 0.2, 0.03}},
    {2030.7, {92.13, 100, 60.66, 25.8, 7.81, 2.29, 0.46, 0.09, 0.03}},
    {2040.651, {93.33, 100, 59.22, 24.31, 8.19, 1.7, 0.52, 0.06, 0.03}},
    {2050.541, {95.83, 100, 56.76, 25.82, 8.02, 2.34, 0.49, 0.17}},
    {2060.621, {86.87, 100, 57.85, 24.44, 9, 2.59, 0.37, 0.06, 0.06}},
    {2070.701, {89.69, 100, 59.2, 25.65, 8.33, 2.61, 0.66, 0.09, 0.03, 0.03}},
    {2080.696, {94.57, 100, 60.64, 27.18, 9.09, 2.74, 0.71, 0.15}},
    {2090.776, {88.51, 100, 58.01, 27.21, 8.27, 2.31, 0.71, 0.14, 0.03, 0.03}},
    {2100.771, {89.26, 100, 61.93, 26.84, 8.62, 2.73, 0.67, 0.15}},
    {2110.722, {85.59, 100, 59.72, 25.19, 9.43, 2.04, 0.74, 0.25, 0.08}},
    {2120.717, {86.73, 100, 61.13, 26.66, 9.59, 2.97, 0.58, 0.2, 0.06, 0.03}},
    {2130.692, {87.79, 100, 64.81, 27.6, 10.03, 3.12, 0.74, 0.12}},
    {2140.497, {86.7, 100, 66.15, 33.27, 12.97, 3.74, 1.22, 0.21, 0.12, 0.03}},
    {2150.767, {84.46, 100, 62.51, 24.88, 8.92, 2.39, 0.62, 0.14, 0.09}},
    {2160.847, {82.22, 100, 59.47, 26.52, 10.01, 2.79, 0.68, 0.14, 0.03}},
    {2170.842, {85.96, 100, 63.26, 27.27, 10.12, 2.82, 1.02, 0.23, 0.03}},
    {2180.922, {85.1, 100, 64.48, 27.13, 8.97, 2.78, 0.72, 0.09}},
    {2190.788, {84.3, 100, 59.47, 27.63, 9.93, 2.85, 0.94, 0.23, 0.03}},
    {2200.868, {86.69, 100, 62.14, 29.64, 9.32, 2.71, 0.55, 0.09, 0.06}},
    {2209.56, {83.97, 100, 66.36, 34.86, 13.46, 4.88, 1.31, 0.34, 0.06}},
    {2220.838, {80.47, 100, 63.25, 29.44, 10.89, 2.88, 0.75, 0.32, 0.03}},
    {2230.833, {81.72, 100, 66.47, 28.3, 10.92, 3.53, 1.05, 0.06}},
    {2240.913, {85.18, 100, 67.89, 29.17, 10.74, 3.6, 0.83, 0.15, 0.03, 0.03}},
    {2249.581, {79.38, 100, 68.6, 34.24, 12.92, 4.62, 1.57, 0.42, 0.15, 0.03}},
    {2259.851, {84.66, 100, 67.85, 29.88, 10.93, 3.67, 1.34, 0.18}},
    {2269.931, {84.84, 100, 68.12, 30.12, 10.78, 3.31, 1.04, 0.18, 0.09, 0.03}},
    {2279.631, {78.75, 100, 68.6, 36.86, 13.99, 4.7, 1.49, 0.37, 0.09, 0.03}},
    {2290.006, {79.9, 100, 64.32, 30.17, 11.29, 3.34, 0.58, 0.44, 0.09}},
    {2299.896, {84.41, 100, 69.48, 32.15, 11.7, 3.29, 0.79, 0.36, 0.03}},
    {2309.976, {78.89, 100, 66.98, 28.8, 12.32, 3.23, 0.67, 0.32}},
    {2319.971, {79.07, 100, 64.63, 32.15, 11.46, 3.86, 0.96, 0.12, 0.06}},
    {2330.051, {77.28, 100, 63.85, 30.73, 11.18, 3.89, 0.81, 0.23, 0.03, 0.03}},
    {2340.002, {77.96, 100, 66.59, 31.6, 11.28, 3.57, 1.2, 0.12}},
    {2349.702, {77.95, 100, 74.69, 38.43, 14.81, 5.04, 1.5, 0.53, 0.19, 0.03}},
    {2360.077, {76.64, 100, 65.79, 32.23, 11.93, 3.85, 1.02, 0.15, 0.03}},
    {2370.072, {79.1, 100, 69.47, 31.51, 11.98, 3.54, 1.22, 0.39, 0.06}},
    {2380.047, {74.44, 100, 66.15, 31.84, 12.79, 4.07, 1.13, 0.23, 0.06, 0.06}},
    {2390.042, {77.29, 100, 69.09, 34.11, 12.48, 4.28, 1.5, 0.39, 0.09}},
    {2400.122, {79.14, 100, 67.54, 34.05, 13.79, 3.75, 1.14, 0.33, 0.03}},
    {2409.798, {75.7, 100, 71.86, 41.49, 16.05, 7.15, 2.08, 0.28, 0.03, 0.13}},
    {2420.068, {76.48, 100, 66.36, 33.55, 12.89, 4.28, 1.18, 0.27, 0.06}},
    {2430.148, {79.06, 100, 68.72, 33.38, 13.38, 3.99, 1.11, 0.27, 0.03}},
    {2439.848, {76.72, 100, 74.47, 37.55, 15.68, 6.04, 1.91, 0.47, 0.06}},
    {2450.223, {74.47, 100, 69.33, 33.55, 13.93, 4.13, 1.19, 0.27, 0.03, 0.03}},
    {2460.113, {75.51, 100, 69.57, 35.55, 12.89, 4.77, 1.05, 0.39, 0.03}},
    {2470.193, {74.98, 100, 71.28, 36.34, 15, 4.28, 1.43, 0.24, 0.03}},
    {2479.869, {74.32, 100, 71.74, 40.85, 17.14, 6.88, 2.42, 0.5, 0.09, 0.03}},
    {2489.864, {73.17, 100, 76.56, 38.55, 16.53, 6.82, 1.79, 0.6, 0.16}},
    {2500.134, {77.23, 100, 75.89, 37.63, 14.52, 5.28, 1.37, 0.34, 0.03}},
    {2510.214, {77.85, 100, 73.48, 36.88, 15.38, 5.02, 1.3, 0.16, 0.09}},
    {2520.294, {73.21, 100, 70.05, 35.73, 15.53, 4.82, 1.35, 0.24, 0.06, 0.03}},
    {2530.289, {74.4, 100, 72.63, 37.23, 13.76, 4.91, 1.74, 0.34, 0.12, 0.03}},
    {2540.264, {72.46, 100, 70.62, 37.12, 14.54, 5.77, 1.42, 0.3, 0.06}},
    {2549.94,
     {70.86, 100, 76.01, 44.35, 18.21, 6.77, 2.14, 0.93, 0.16, 0.03, 0.03}},
    {2559.935, {75.32, 100, 76.2, 45.15, 19.49, 7.08, 2.25, 0.85, 0.1, 0.03}},
    {2570.015,
     {72.32, 100, 76.46, 43.38, 18.1, 7.33, 2.67, 1, 0.23, 0.03, 0.03}},
    {2580.285, {69.88, 100, 73.93, 37.91, 15.55, 5.17, 1.49, 0.27, 0.03}},
    {2589.985, {72.14, 100, 81.58, 44.05, 19.31, 7.41, 2.56, 0.59, 0.1, 0.03}},
    {2600.36, {69.88, 100, 73.71, 37.94, 14.57, 5.51, 2.04, 0.49, 0.06, 0.03}},
    {2610.44, {74.22, 100, 77.08, 41.96, 16.91, 5.94, 1.27, 0.38, 0.1}},
    {2620.011,
     {73, 100, 81.72, 44.08, 20.4, 8.66, 2.82, 0.66, 0.2, 0.03, 0.03, 0.07}},
    {2630.41, {73.88, 100, 80.03, 39.32, 15.7, 5.67, 1.39, 0.54, 0.03}},
    {2640.086, {71.2, 100, 77.8, 44.33, 21.41, 8.83, 2.52, 0.56, 0.23, 0.03}},
    {2650.081, {65.97, 100, 75.86, 44.78, 20.36, 7.07, 2.06, 0.79, 0.22, 0.03}},
    {2660.161, {71.3, 100, 79.46, 44.66, 19.52, 7.64, 2.84, 0.75, 0.29}},
    {2670.241, {72.39, 100, 83.63, 46.53, 21.57, 7.72, 2.38, 1.07, 0.07, 0.1}},
    {2680.236,
     {70.27, 100, 80.28, 45.13, 20.77, 7.81, 2.99, 0.66, 0.23, 0.03, 0.03}},
    {2690.316, {67.46, 100, 81, 46.99, 20.35, 8.32, 2.42, 0.79, 0.26, 0.07}},
    {2700.396, {67.69, 100, 76.82, 42.59, 20.69, 7.34, 3.13, 0.77, 0.19, 0.06}},
    {2710.157, {65.24, 100, 80.8, 48.33, 20.64, 8.72, 2.69, 0.95, 0.26, 0.03}},
    {2720.152,
     {70.37, 100, 83.23, 46.16, 21.28, 9.22, 2.27, 1.04, 0.37, 0.03, 0.03}},
    {2730.232, {70.63, 100, 83.08, 47.07, 23.32, 8.55, 2.79, 0.81, 0.24}},
    {2740.227, {67.95, 100, 83.49, 49.75, 22.83, 9.66, 3.21, 0.68, 0.1, 0.07}},
    {2750.307,
     {67.94, 100, 81.04, 48.92, 22.79, 8.23, 3.07, 0.8, 0.23, 0.17, 0.03}},
    {2760.387, {72.68, 100, 83.85, 48.66, 21.65, 8.21, 2.88, 1.12, 0.17, 0.1}},
    {2770.382, {69.3, 100, 82.71, 50.61, 23.66, 9.84, 3.2, 0.61, 0.44}},
    {2780.462, {63.62, 100, 81.93, 46.59, 23.31, 7.83, 2.91, 0.88, 0.26, 0.1}},
    {2790.223, {65.82, 100, 85.05, 49.6, 23.03, 8.99, 2.93, 0.98, 0.3}},
    {2800.303, {70.61, 100, 86.75, 51.53, 25.28, 9.87, 3.42, 0.87, 0.31, 0.03}},
    {2810.298, {67.71, 100, 85.76, 49.69, 22.74, 9.25, 3.13, 1.33, 0.24, 0.07}},
    {2820.378,
     {66.26, 100, 83.95, 48.07, 23.13, 9.9, 3.22, 0.77, 0.23, 0.1, 0.03}},
    {2830.458, {65.74, 100, 86.54, 51.67, 20.7, 10.74, 3.2, 0.88, 0.41, 0.03}},
    {2840.453,
     {66.79, 100, 87.54, 49.66, 24.4, 10.98, 3.41, 0.93, 0.31, 0.07, 0.03}},
    {2850.533, {65.08, 100, 84.12, 51.75, 24.62, 9.69, 3.26, 1.12, 0.34, 0.03}},
    {2860.294, {65.92, 100, 84.11, 49.9, 24.68, 9.3, 3.08, 0.74, 0.3, 0.03}},
    {2870.289,
     {61.96, 100, 84.29, 50.18, 24.89, 10.33, 3.36, 1.18, 0.13, 0.03}},
    {2880.369,
     {60.28, 100, 82.93, 50.02, 23.71, 10.16, 3.65, 0.86, 0.4, 0.07, 0.03}},
    {2890.449,
     {66.52, 100, 84.12, 54.04, 25.51, 10.29, 4.61, 1.04, 0.42, 0.03, 0.03}},
    {2900.315, {61.02, 100, 82.32, 51.71, 24.85, 9.98, 3.25, 1.31, 0.44, 0.03}},
    {2910.524, {62.03, 100, 85.49, 52.17, 24.03, 10.37, 3.32, 1.22, 0.34}},
    {2920.604, {65.73, 100, 88.02, 52.91, 25.81, 10.34, 3.73, 1.5, 0.21, 0.07}},
    {2930.365,
     {60.55, 100, 89.31, 54.77, 27.4, 10.9, 3.45, 1.15, 0.52, 0.07, 0.07}},
    {2940.36,
     {66.74, 100, 89.96, 56.27, 27.31, 11.43, 4.91, 1.22, 0.39, 0.07, 0.07,
      0.04}},
    {2950.44, {61.15, 100, 86.91, 55.47, 25.87, 11.22, 3.81, 1.35, 0.38, 0.1}},
    {2960.435,
     {64.99, 100, 88.02, 54.35, 27.83, 12.01, 4.02, 0.67, 0.28, 0.04, 0.04}},
    {2970.515, {59.63, 100, 85.14, 52.52, 25.75, 11.02, 4.15, 1.53, 0.31, 0.1}},
    {2980.466, {60.93, 100, 89.59, 55.61, 27.05, 11.21, 4.27, 1.3, 0.35, 0.07}},
    {2990.675,
     {61.05, 100, 85.37, 53.75, 27.27, 11.47, 4.1, 0.72, 0.48, 0.1, 0.03}},
    {3000.436,
     {59.9, 100, 86.61, 52.47, 25.45, 12.4, 3.8, 1.23, 0.41, 0.14, 0.03, 0.03}},
    {3010.621,
     {60.28, 100, 85.97, 56.77, 26.94, 10.8, 4.69, 1.08, 0.45, 0.17, 0.07}},
    {3020.511,
     {60.6, 100, 90.38, 57.29, 28.96, 12.79, 4.35, 1.35, 0.39, 0.07, 0.07}},
    {3030.506,
     {63.52, 100, 87.01, 56.16, 25.91, 11.83, 3.71, 1.37, 0.53, 0.07, 0.04}},
    {3040.586, {61.81, 100, 91.68, 55.29, 30.05, 13.02, 4.3, 1.69, 0.61, 0.11}},
    {3050.666,
     {58.44, 100, 90.7, 54.09, 29.41, 11.72, 4.49, 1.44, 0.56, 0.07, 0.07}},
    {3060.532,
     {57.08, 100, 88.23, 57.32, 30.87, 12.43, 3.86, 1.09, 0.32, 0.04}},
    {3070.507,
     {60.28, 100, 88.81, 60.46, 30.25, 12.09, 4.77, 1.61, 0.36, 0.07, 0.07,
      0.04}},
    {3079.813,
     {58.64, 100, 87.92, 59.95, 29.07, 12.54, 4.39, 1.13, 0.39, 0.04, 0.04}},
    {3089.574, {60.6, 100, 91.08, 59.41, 31.27, 11.56, 5.06, 1.66, 0.36, 0.14}},
    {3099.569,
     {60.79, 100, 87.81, 57.31, 30.39, 12.48, 4.41, 1.49, 0.68, 0.14}},
    {3109.649,
     {58.63, 100, 90.97, 60.71, 32.81, 13.55, 4.99, 1.82, 0.66, 0.04}},
    {3119.644,
     {58.37, 100, 93.94, 57.18, 30.16, 14.25, 5.16, 1.33, 0.29, 0.07}},
    {3129.595,
     {60.47, 100, 91.96, 64.56, 29.83, 14.23, 5.64, 1.47, 0.44, 0.11}},
    {3139.59, {57.07, 100, 93.51, 59.5, 30.57, 13.89, 5.66, 1.63, 0.65, 0.11}},
    {3149.67, {61.67, 100, 95.17, 63.36, 32.82, 13.19, 5.99, 1.87, 0.49, 0.11}},
    {3159.645,
     {57.37, 100, 89.4, 57.23, 28.31, 14.03, 5.28, 1.95, 0.5, 0.25, 0.04}},
    {3169.64, {59.87, 100, 92.14, 59.39, 33.85, 14.77, 4.89, 1.8, 0.55, 0.26}},
    {3179.72,
     {54.44, 100, 88.87, 58.94, 30.21, 13.35, 4.4, 1.34, 0.39, 0.14, 0.04}},
    {3189.715, {54.37, 100, 92.16, 61.85, 33.54, 13.54, 5.26, 1.6, 0.51, 0.15}},
    {3199.666,
     {59.01, 100, 95.7, 64.99, 33.33, 12.71, 5.23, 2.17, 0.34, 0.19, 0.04}},
    {3209.661,
     {58.25, 100, 96.03, 61.85, 31.24, 15.9, 5.2, 2.19, 0.63, 0.15, 0.04}},
    {3219.741,
     {59.83, 100, 99.92, 65.82, 33.35, 14.02, 5.18, 2.06, 0.76, 0.15}},
    {3229.736,
     {58.9, 100, 97.61, 65.08, 34.58, 14.43, 5.27, 1.93, 0.64, 0.23, 0.08,
      0.04}},
    {3239.711,
     {55.21, 100, 93.87, 64.15, 32.48, 16.26, 4.95, 2.11, 0.37, 0.11, 0.04}},
    {3249.791,
     {53.55, 100, 91.71, 62.24, 32.19, 13.9, 5.87, 1.63, 0.58, 0.29, 0.11}},
    {3259.786,
     {57.8, 100, 94.94, 65.32, 35.59, 15.87, 5.74, 1.81, 0.57, 0.08, 0.08}},
    {3269.866,
     {56.44, 100, 93.68, 65.42, 34.81, 15.12, 5.88, 2.21, 0.52, 0.19}},
    {3279.946,
     {56.04, 100, 97.73, 65.85, 38.58, 17.54, 6.15, 1.81, 0.73, 0.12, 0.04,
      0.04}},
    {3289.812,
     {52.72, 100, 95.79, 61.36, 35.54, 15.59, 5.58, 1.85, 0.7, 0.26, 0.04}},
    {3299.807,
     {56.38, 100, 94.85, 63.61, 32.81, 16.96, 5.82, 1.64, 0.48, 0.15, 0.07,
      0.04, 0.04}},
    {3309.887, {58.07, 97.8, 100, 68.71, 39.62, 18.3, 7.03, 2.36, 0.59, 0.16}},
    {3319.967,
     {54.94, 100, 96.81, 64.48, 38.11, 17.14, 5.74, 1.82, 0.65, 0.23, 0.04}},
    {3329.857,
     {54.94, 100, 97.29, 67.7, 36.73, 15.88, 6.3, 2.18, 0.57, 0.15, 0.04,
      0.04}},
    {3339.937,
     {54.66, 100, 97.18, 67.25, 36.83, 16.72, 5.99, 1.91, 0.8, 0.27, 0.08}},
    {3350.122,
     {54.53, 98, 100, 66.39, 38.53, 15.66, 7.18, 2.61, 0.61, 0.15, 0.08}},
    {3359.883,
     {55.16, 100, 98.8, 68.77, 36.45, 17.7, 6.61, 2.13, 0.73, 0.12, 0.08}},
    {3369.878,
     {52.92, 100, 96.24, 66.43, 36.68, 17.22, 6.3, 2.35, 1.1, 0.08, 0.04}},
    {3379.958,
     {51.35, 100, 98.73, 69.41, 37.2, 17.95, 6.42, 2.31, 0.58, 0.31, 0.04,
      0.04}},
    {3389.953,
     {54.14, 100, 99.65, 71.65, 39.38, 17.35, 6.64, 2.91, 0.67, 0.12, 0.08,
      0.04}},
    {3399.928,
     {54.03, 98.91, 100, 71.03, 39.72, 16.69, 6.96, 2.58, 0.51, 0.43, 0.04,
      0.04}},
    {3409.923,
     {54.63, 100, 95.16, 68.28, 39.25, 17.15, 6.04, 2.92, 0.65, 0.27, 0.08,
      0.04}},
    {3420.003, {53.59, 98.15, 100, 67.09, 37.75, 17.45, 7, 2.54, 0.85, 0.04}},
    {3429.954,
     {53.52, 100, 95.65, 72.87, 39.64, 17.76, 6.41, 2.18, 0.39, 0.23}},
    {3439.949,
     {52.28, 100, 95.78, 65.54, 39.89, 16.53, 6.57, 2.51, 0.65, 0.15, 0.04}},
    {3450.029, {52.69, 96.92, 100, 70.15, 39.87, 19.25, 6.94, 2.69, 0.9, 0.31}},
    {3460.024,
     {52.04, 97.44, 100, 71.42, 39.15, 17.44, 7.03, 2.41, 0.78, 0.58, 0.08}},
    {3470.104,
     {54.18, 100, 99.96, 71.96, 40.54, 18.84, 7.42, 2.38, 0.99, 0.36, 0.04}},
    {3479.994,
     {54.46, 100, 98.04, 69.77, 39.3, 18.85, 8.28, 2.59, 0.9, 0.27, 0.08, 0.04,
      0.04}},
    {3490.074,
     {51.88, 97.23, 100, 72.33, 36.24, 17.03, 6.11, 2.19, 1.04, 0.12, 0.12,
      0.04}},
    {3500.259,
     {54.7, 99.84, 100, 70.24, 40.51, 19.17, 7.55, 2.02, 0.59, 0.4, 0.24}},
    {3510.02,
     {51.21, 100, 96.63, 74.93, 41.09, 20.83, 7.54, 2.98, 1.07, 0.32, 0.08}},
    {3520.1, {50, 94.37, 100, 67.19, 39.09, 18.61, 8.81, 3.37, 1, 0.31, 0.11}},
    {3530.095,
     {51.68, 93.16, 100, 70.35, 41.05, 18.28, 8.04, 2.59, 1.01, 0.27, 0.08,
      0.04}},
    {3540.175,
     {52.87, 96.07, 100, 76.29, 43.76, 19.94, 7.34, 3.45, 1, 0.32, 0.04, 0.04}},
    {3550.17,
     {50.55, 97.73, 100, 71.55, 39.97, 19.2, 8.66, 2.7, 0.86, 0.31, 0.31}},
    {3560.25,
     {50.44, 94.08, 100, 68.04, 40.57, 19.4, 7.18, 3.15, 1.04, 0.19, 0.04,
      0.04}},
    {3570.14,
     {47.68, 94.32, 100, 70.81, 40.04, 20.54, 8.38, 3.01, 0.89, 0.35, 0.04,
      0.04}},
    {3580.091,
     {51.42, 99.88, 100, 74.85, 41.74, 19.47, 8.2, 3.16, 0.88, 0.12, 0.12}},
    {3590.405,
     {46.74, 93.56, 100, 70.92, 38.89, 20.11, 8.7, 2.87, 1.03, 0.27, 0.04}},
    {3600.166,
     {48.92, 93.09, 100, 72.63, 39.69, 19.34, 8.3, 2.9, 1, 0.15, 0.04, 0.04}},
    {3610.246,
     {50.94, 99.68, 100, 75.43, 42.98, 20.71, 8.24, 2.69, 1.01, 0.2, 0.16,
      0.04}},
    {3620.241,
     {50.14, 93.13, 100, 71.36, 41.02, 20.18, 8.15, 2.76, 0.74, 0.43, 0.16}},
    {3630.321,
     {49.49, 93.5, 100, 72.61, 42.24, 21.39, 8.07, 3.06, 1.14, 0.27, 0.04,
      0.04}},
    {3640.316,
     {47.69, 93.32, 100, 72.2, 43.98, 20.64, 8.37, 3.4, 0.9, 0.39, 0.04}},
    {3650.162,
     {49.14, 100, 99.88, 78.62, 47.01, 21.79, 9.05, 2.91, 0.82, 0.2, 0.08}},
    {3660.476,
     {47.36, 91.87, 100, 75.24, 42.2, 20.88, 8.02, 3.79, 1.41, 0.27, 0.04}},
    {3670.237,
     {47.74, 92.66, 100, 71.86, 40.16, 19.98, 8.74, 3.56, 1.28, 0.39, 0.08,
      0.08, 0.04}},
    {3680.232,
     {47.77, 95.81, 100, 73.33, 41.53, 21.81, 9.72, 3.63, 1.07, 0.24, 0.16,
      0.04}},
    {3690.312,
     {44.13, 95.25, 100, 76.15, 42.63, 22.24, 8.21, 2.83, 1.22, 0.28}},
    {3700.392,
     {46.08, 91.27, 100, 75.53, 43.74, 21.65, 8.93, 2.66, 1.06, 0.51, 0.08,
      0.04}},
    {3710.387, {46.14, 94.97, 100, 76.59, 42.65, 20.95, 9.5, 3.52, 1.43, 0.28}},
    {3720.467,
     {44.56, 92.3, 100, 74.56, 45.19, 21.48, 9.31, 3.57, 1.14, 0.39, 0.12}},
    {3730.547,
     {46.8, 93.31, 100, 77.66, 43.35, 23.82, 10.29, 3.28, 0.92, 0.52, 0.32,
      0.04}},
    {3740.308,
     {47.35, 91.58, 100, 74.51, 45.38, 20.43, 10.12, 4.11, 1.23, 0.43, 0.08,
      0.04}},
    {3750.303,
     {47.08, 93.53, 100, 76.66, 41.89, 23.94, 10.67, 3.64, 1.28, 0.76, 0.2,
      0.04}},
    {3760.383,
     {45.6, 94.71, 100, 78.49, 46.73, 22.24, 9.44, 3.87, 1.74, 0.44, 0.16,
      0.12}},
    {3770.378,
     {47.08, 89.57, 100, 74.53, 45.26, 23.42, 9.6, 3.95, 0.91, 0.43, 0.16,
      0.04}},
    {3780.458,
     {43.28, 90.68, 100, 76.44, 47.68, 22.17, 10.39, 4.32, 1.03, 0.48, 0.16,
      0.04}},
    {3790.538,
     {45, 95.77, 100, 76.94, 45.93, 24.07, 9.52, 4.19, 1.21, 0.4, 0.16, 0.04}},
    {3800.404,
     {46.08, 92.12, 100, 79.6, 47.09, 23.42, 9.98, 3.68, 1.09, 0.61, 0.2}},
    {3810.379,
     {43.46, 94.51, 100, 78.37, 48.51, 23.37, 9.08, 4.28, 1.45, 0.36, 0.16}},
    {3820.374,
     {46.84, 93.34, 100, 78.77, 47.9, 24.5, 10.53, 4.21, 1.51, 0.61, 0.12}},
    {3830.454,
     {43.94, 90.36, 100, 77.89, 46.69, 22.87, 10.2, 4.38, 1.39, 0.4, 0.2,
      0.08}},
    {3840.449,
     {43.72, 87.71, 100, 75.23, 47.42, 23.28, 10.36, 4.61, 1.1, 0.32, 0.12}},
    {3850.529,
     {42.91, 92.23, 100, 79.41, 48.52, 22.48, 9.33, 3.89, 1.04, 0.68, 0.12,
      0.04}},
    {3860.524,
     {41.57, 88.3, 100, 78.25, 45.51, 23.6, 10.76, 4.1, 1.34, 0.28, 0.2, 0.08,
      0.04}},
    {3870.604,
     {44.01, 87.23, 100, 75.41, 47.12, 24.39, 9.93, 3.74, 1.26, 0.71, 0.12,
      0.08}},
    {3880.555,
     {43.63, 88.56, 100, 82.44, 47.36, 24.78, 12.17, 4.1, 1.91, 0.32, 0.16,
      0.04, 0.04}},
    {3890.445,
     {43.81, 92.49, 100, 82.16, 50.21, 26.34, 11.35, 4.75, 1.28, 0.41, 0.04,
      0.04}},
    {3900.525,
     {38.7, 92.48, 100, 80.41, 49.24, 24.3, 10.5, 4.55, 1.53, 0.36, 0.12,
      0.08}},
    {3910.52,
     {41.26, 87.71, 100, 77.91, 46.17, 25.22, 10.71, 4.51, 0.99, 0.51, 0.16,
      0.08, 0.04}},
    {3920.6,
     {42.03, 91.87, 100, 75.36, 49.04, 24.88, 11.26, 4.25, 1.52, 0.4, 0.04}},
    {3930.595,
     {41.83, 89.94, 100, 78.08, 47.99, 26.55, 11.06, 4.14, 2.21, 0.4, 0.04}},
    {3940.675,
     {40.48, 87.62, 100, 78.2, 52.14, 24.21, 11.54, 4.13, 1.8, 0.48, 0.16,
      0.04}},
    {3950.541,
     {40.36, 90.73, 100, 80.11, 49.75, 29.17, 11.4, 5.23, 1.18, 0.41, 0.12,
      0.04}},
    {3960.621,
     {43.11, 92.13, 100, 82.98, 49.57, 27.41, 11.97, 4.68, 1.7, 0.37, 0.08,
      0.08}},
    {3969.588,
     {41.8, 86.39, 100, 78.25, 51.87, 25.9, 11.84, 4.27, 1.73, 0.52, 0.16}},
    {3979.583,
     {39.34, 88.42, 100, 77.24, 48.6, 26, 11.58, 5.31, 2.16, 0.56, 0.12, 0.04}},
    {3989.663,
     {39.83, 89.06, 100, 78.97, 52.26, 24.9, 12.07, 4.32, 1.57, 0.44, 0.08,
      0.04}},
    {3999.848,
     {42.17, 89.57, 100, 78.98, 49.11, 27.68, 11.32, 4.83, 1.46, 0.45, 0.16,
      0.12}},
};
std::vector<FeatureDetection::Feature> FeatureDetection::detect_features(
    const std::vector<Centroid::Peak> &peaks,
    const std::vector<uint8_t> &charge_states) {
    double carbon_diff = 1.0033;  // NOTE: Maxquant uses 1.00286864

    // Sort peaks by mz.
    auto sorted_peaks = std::vector<Search::KeySort<double>>(peaks.size());
    for (size_t i = 0; i < peaks.size(); ++i) {
        sorted_peaks[i] = {i, peaks[i].fitted_mz};
    }
    std::stable_sort(
        sorted_peaks.begin(), sorted_peaks.end(),
        [](auto &p1, auto &p2) { return (p1.sorting_key < p2.sorting_key); });

    // Initialize graph.
    std::vector<FeatureDetection::CandidateGraph> charge_state_graphs(
        charge_states.size());
    for (size_t k = 0; k < charge_states.size(); ++k) {
        charge_state_graphs[k].resize(sorted_peaks.size());
    }
    for (size_t i = 0; i < sorted_peaks.size(); ++i) {
        auto &ref_peak = peaks[sorted_peaks[i].index];
        double tol_mz = ref_peak.fitted_sigma_mz;
        double tol_rt = ref_peak.fitted_sigma_rt;
        double min_rt = ref_peak.fitted_rt - tol_rt;
        double max_rt = ref_peak.fitted_rt + tol_rt;
        for (size_t k = 0; k < charge_states.size(); ++k) {
            charge_state_graphs[k][i].id = ref_peak.id;
            auto charge_state = charge_states[k];
            if (charge_state == 0) {
                continue;
            }
            double mz_diff = carbon_diff / charge_state;
            double min_mz = (ref_peak.fitted_mz + mz_diff) - tol_mz;
            double max_mz = (ref_peak.fitted_mz + mz_diff) + tol_mz;

            // Find peaks within tolerance range and add them to the graph.
            for (size_t j = i + 1; j < sorted_peaks.size(); ++j) {
                auto &peak = peaks[sorted_peaks[j].index];
                if (peak.fitted_mz > max_mz) {
                    break;
                }
                if (peak.fitted_mz > min_mz && peak.fitted_rt > min_rt &&
                    peak.fitted_rt < max_rt) {
                    charge_state_graphs[k][i].nodes.push_back(j);
                }
            }
        }
    }

    // Visit nodes to find most likely features.
    std::vector<FeatureDetection::Feature> features;
    size_t i = 0;
    while (i < sorted_peaks.size()) {
        double best_dot = 0.0;
        std::vector<uint64_t> best_path;
        uint8_t best_charge_state = 0;
        for (size_t k = 0; k < charge_states.size(); ++k) {
            int64_t charge_state = charge_states[k];
            auto paths = find_all_paths(charge_state_graphs[k], i);
            double ref_mz =
                peaks[sorted_peaks[i].index].fitted_mz * charge_states[k];
            auto ref_key_it = averagine_table.lower_bound(ref_mz);
            if (ref_key_it == averagine_table.end()) {
                continue;
            }
            // Check if the next element is closer to the ref_mz.
            auto ref_key_it_next = ref_key_it;
            ref_key_it_next++;
            if (ref_key_it_next != averagine_table.end() &&
                ref_mz - ref_key_it->first > ref_key_it_next->first - ref_mz) {
                ref_key_it++;
            }
            std::vector<double> &ref_heights = ref_key_it->second;
            for (const auto &path : paths) {
                if (path.size() < 2) {
                    continue;
                }
                std::vector<double> path_heights;
                for (const auto &x : path) {
                    path_heights.push_back(
                        peaks[sorted_peaks[x].index].fitted_height);
                }
                auto sim =
                    rolling_weighted_cosine_sim(path_heights, ref_heights);
                if (sim.dot > best_dot) {
                    best_dot = sim.dot;
                    best_charge_state = charge_state;
                    best_path = std::vector<size_t>(sim.max_i - sim.min_i);
                    size_t k = 0;
                    for (size_t i = sim.min_i; i < sim.max_i; ++i, ++k) {
                        best_path[k] = path[i];
                    }
                }
            }
        }
        if (best_path.size() < 2) {
            ++i;
            continue;
        }
        // We found a feature, but the initial reference is not included.
        if (best_path[0] == i) {
            ++i;
        }

        // Build feature.
        FeatureDetection::Feature feature = {};
        feature.score = best_dot;
        feature.charge_state = best_charge_state;
        feature.average_rt = 0.0;
        feature.average_rt_sigma = 0.0;
        feature.average_rt_delta = 0.0;
        feature.average_mz = 0.0;
        feature.average_mz_sigma = 0.0;
        feature.total_height = 0.0;
        feature.total_volume = 0.0;
        feature.max_height = 0.0;
        feature.max_volume = 0.0;
        feature.peak_ids = std::vector<uint64_t>(best_path.size());
        for (size_t i = 0; i < best_path.size(); ++i) {
            const auto &graph_idx = best_path[i];
            const auto &peak = peaks[sorted_peaks[graph_idx].index];

            feature.peak_ids[i] = peak.id;
            feature.average_rt += peak.fitted_rt;
            feature.average_rt_sigma += peak.fitted_sigma_rt;
            feature.average_rt_delta += peak.rt_delta;
            feature.average_mz += peak.fitted_mz * peak.fitted_height;
            feature.average_mz_sigma += peak.fitted_sigma_mz;
            feature.total_height += peak.fitted_height;
            feature.total_volume += peak.fitted_volume;
            if (peak.fitted_height > feature.max_height) {
                feature.max_height = peak.fitted_height;
            }
            if (peak.fitted_volume > feature.max_volume) {
                feature.max_volume = peak.fitted_volume;
            }
            if (i == 0) {
                feature.monoisotopic_mz = peak.fitted_mz;
                feature.monoisotopic_rt = peak.fitted_rt;
                feature.monoisotopic_height = peak.fitted_height;
                feature.monoisotopic_volume = peak.fitted_volume;
            }

            // Mark peaks as used.
            for (size_t k = 0; k < charge_states.size(); ++k) {
                charge_state_graphs[k][graph_idx].visited = true;
            }
        }
        feature.average_rt /= best_path.size();
        feature.average_rt_delta /= best_path.size();
        feature.average_rt_sigma /= best_path.size();
        feature.average_mz /= feature.total_height;
        feature.average_mz_sigma /= best_path.size();

        features.push_back(feature);
    }

    // Sort features and assign ids.
    auto sort_features = [](const auto &a, const auto &b) {
        return (a.total_volume >= b.total_volume);
    };
    std::sort(features.begin(), features.end(), sort_features);
    for (size_t i = 0; i < features.size(); ++i) {
        auto &feature = features[i];
        feature.id = i;
    }

    return features;
}
