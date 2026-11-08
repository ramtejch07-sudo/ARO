#pragma once
#include <vector>

// Optimal assignment (Hungarian / Kuhn-Munkres algorithm, O(n^2 * m)).
// `cost[r][c]` is the cost of giving row r to column c. Every row may have a different number of
// candidate columns: the matrix may be rectangular (rows != cols). Exactly min(rows, cols) pairs
// are formed and their total cost is minimal. Returns, for each row, the assigned column or -1
// when there are more rows than columns. Non-finite costs are treated as a very large cost.
std::vector<int> solveAssignment(const std::vector<std::vector<double>>& cost);
