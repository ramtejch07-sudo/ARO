#include "Hungarian.h"
#include <cmath>
#include <limits>

namespace {

constexpr double kHuge = 1e12;

// Requires rows <= cols. Classic potentials-based Hungarian algorithm (1-indexed internally).
std::vector<int> solveRowsLeCols(const std::vector<std::vector<double>>& a, size_t n, size_t m) {
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> u(n + 1, 0.0), v(m + 1, 0.0);
    std::vector<size_t> p(m + 1, 0), way(m + 1, 0);
    for (size_t i = 1; i <= n; ++i) {
        p[0] = i;
        size_t j0 = 0;
        std::vector<double> minv(m + 1, INF);
        std::vector<bool> used(m + 1, false);
        do {
            used[j0] = true;
            size_t i0 = p[j0], j1 = 0;
            double delta = INF;
            for (size_t j = 1; j <= m; ++j) {
                if (used[j]) continue;
                double cur = a[i0 - 1][j - 1] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (size_t j = 0; j <= m; ++j) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            size_t j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }
    std::vector<int> rowToCol(n, -1);
    for (size_t j = 1; j <= m; ++j) if (p[j] != 0) rowToCol[p[j] - 1] = static_cast<int>(j - 1);
    return rowToCol;
}

} // namespace

std::vector<int> solveAssignment(const std::vector<std::vector<double>>& cost) {
    const size_t rows = cost.size();
    if (rows == 0) return {};
    const size_t cols = cost[0].size();
    if (cols == 0) return std::vector<int>(rows, -1);

    auto clean = [](double c) { return std::isfinite(c) ? c : kHuge; };

    if (rows <= cols) {
        std::vector<std::vector<double>> a(rows, std::vector<double>(cols));
        for (size_t r = 0; r < rows; ++r)
            for (size_t c = 0; c < cols; ++c) a[r][c] = clean(cost[r][c]);
        return solveRowsLeCols(a, rows, cols);
    }
    // More rows than columns: solve the transpose, then map back.
    std::vector<std::vector<double>> t(cols, std::vector<double>(rows));
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c) t[c][r] = clean(cost[r][c]);
    std::vector<int> colToRow = solveRowsLeCols(t, cols, rows);
    std::vector<int> out(rows, -1);
    for (size_t c = 0; c < cols; ++c) if (colToRow[c] >= 0) out[static_cast<size_t>(colToRow[c])] = static_cast<int>(c);
    return out;
}
