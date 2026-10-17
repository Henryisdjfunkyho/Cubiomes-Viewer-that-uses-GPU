#ifndef HIPREFERENCE_H
#define HIPREFERENCE_H

// Conservative, deterministic resolver for the fixed-location subset of the
// Viewer reference graph.  This header intentionally does not evaluate a
// condition or move predicates across NOT/OR/spiral nodes.  Callers use it to
// decide whether a GPU predicate may be emitted at a statically known origin;
// false means "leave the original CPU tree in charge".

#include "search.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace hipreference {

struct FixedOrigin
{
    int64_t x = 0;
    int64_t z = 0;
    int dimension = DIM_OVERWORLD;
};

// Shape-only description of the dynamic part of a biome-center reference.
// The GPU compiler consumes these node indices to build a fused parent/child
// program; the CPU remains authoritative for every surviving seed.
struct DynamicCenterPointGroup
{
    int parentNode = -1;
    std::vector<int> childNodes;
};

inline bool addChecked(int64_t a, int64_t b, int64_t *out)
{
    if (!out)
        return false;
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
        return false;
    *out = a + b;
    return true;
}

inline bool multiplyEightChecked(int64_t value, int64_t *out)
{
    if (!out || value > std::numeric_limits<int64_t>::max() / 8 ||
        value < std::numeric_limits<int64_t>::min() / 8)
        return false;
    *out = value * 8;
    return true;
}

// The C++ CPU evaluator uses ordinary signed integer division for
// F_SCALE_TO_NETHER (truncation toward zero), not mathematical floor.
inline int64_t truncateDivideEight(int64_t value)
{
    return value / 8;
}

// Climate locate returns a quart-aligned block coordinate.  Unlike the scale
// helper above, this is a floor operation for negative coordinates because the
// CPU path effectively uses (x >> 2) << 2.
inline int64_t floorToQuart(int64_t value)
{
    return value / 4 - (value % 4 < 0 ? 1 : 0);
}

inline int64_t floorDivide(int64_t value, int64_t divisor)
{
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}

inline bool multiplyScaleChecked(int64_t value, int64_t scale, int64_t *out)
{
    if (!out || scale <= 0 || value > std::numeric_limits<int64_t>::max() / scale ||
        value < std::numeric_limits<int64_t>::min() / scale)
        return false;
    *out = value * scale;
    return true;
}

inline bool biomeCenterPoint(const Condition &condition, int mc, bool full64,
    int64_t originX, int64_t originZ, int dimension, int64_t *x, int64_t *z)
{
    // A single 1:4 cell is the only modern center case whose output position
    // is fixed without running biome generation. Keep the parent's actual
    // biome/size/tolerance test on the CPU; this only resolves its geometry.
    if (condition.type != F_BIOME_CENTER || mc < MC_1_18 || !full64 ||
        condition.count != 1 || condition.skipref || condition.rmax > 0 ||
        dimension != DIM_OVERWORLD || !x || !z)
        return false;

    int64_t x1, x2, z1, z2;
    if (!addChecked(originX, condition.x1, &x1) ||
        !addChecked(originX, condition.x2, &x2) ||
        !addChecked(originZ, condition.z1, &z1) ||
        !addChecked(originZ, condition.z2, &z2) || x2 < x1 || z2 < z1)
        return false;

    const int64_t cellX1 = floorDivide(x1, 4);
    const int64_t cellX2 = floorDivide(x2, 4);
    const int64_t cellZ1 = floorDivide(z1, 4);
    const int64_t cellZ2 = floorDivide(z2, 4);
    if (cellX1 != cellX2 || cellZ1 != cellZ2)
        return false;

    int64_t centerX, centerZ;
    if (!multiplyScaleChecked(cellX1, 4, &centerX) ||
        !multiplyScaleChecked(cellZ1, 4, &centerZ) ||
        !addChecked(centerX, 2, &centerX) || !addChecked(centerZ, 2, &centerZ))
        return false;
    *x = centerX;
    *z = centerZ;
    return true;
}

inline bool validNode(const ConditionTree &tree, int node)
{
    return node >= 0 && size_t(node) < tree.condvec.size() &&
        tree.condvec[node].save == node &&
        !(tree.condvec[node].meta & Condition::DISABLED);
}

inline bool isSupportedClimateParameter(uint8_t parameter)
{
    return parameter == NP_TEMPERATURE || parameter == NP_HUMIDITY ||
        parameter == NP_EROSION || parameter == NP_CONTINENTALNESS ||
        parameter == NP_WEIRDNESS;
}

// Validate the safe direct-parent shape for a dynamic center -> point-climate
// group. For count==1 the CPU locator requests at most one center; the
// dependent children are ANDed at that first returned position. childNodes
// therefore preserves the tree's branch order. No child is silently omitted:
// any unsupported or nested condition rejects the whole group.
//
// This is only a lowering eligibility check, not a replacement for CPU
// evaluation. A GPU caller must either produce the same first center and
// evaluate all listed child predicates there, or use a conservative superset
// that can only retain extra seeds. Ambiguous/overflow cases must be kept for
// CPU replay.
inline bool resolveDynamicCenterPointGroup(const ConditionTree &tree, int parentNode,
    int mc, bool full64, DynamicCenterPointGroup *out)
{
    if (!out || !full64 || mc < MC_1_18 || !validNode(tree, parentNode))
        return false;

    const Condition &parent = tree.condvec[size_t(parentNode)];
    if (parent.type != F_BIOME_CENTER || parent.count != 1 || parent.skipref ||
        parent.rmax > 0 || size_t(parentNode) >= tree.references.size())
        return false;

    const std::vector<char> &children = tree.references[size_t(parentNode)];
    if (children.empty() || children.size() > 8)
        return false;

    DynamicCenterPointGroup group;
    group.parentNode = parentNode;
    group.childNodes.reserve(children.size());
    for (char childIndex : children)
    {
        const int node = int(childIndex);
        if (!validNode(tree, node) || size_t(node) >= tree.references.size())
            return false;
        const Condition &child = tree.condvec[size_t(node)];
        const uint8_t testedBounds = child.minmax &
            (Condition::E_TEST_LOWER | Condition::E_TEST_UPPER);
        if (child.relative != parentNode || child.type != F_CLIMATE_MINMAX ||
            !tree.references[size_t(node)].empty() || child.rmax > 0 ||
            child.x1 != child.x2 || child.z1 != child.z2 ||
            !isSupportedClimateParameter(child.para) ||
            // A center parent performs biome generation before its children.
            // CPU init4Noise then reuses the full generated noise cache; a
            // selected-octave child would evaluate a different field on GPU.
            child.octave != 0 ||
            !(child.minmax & (Condition::E_LOCATE_MIN | Condition::E_LOCATE_MAX)) ||
            (child.flags & Condition::FLG_INVERT) ||
            testedBounds == 0 ||
            (testedBounds == (Condition::E_TEST_LOWER | Condition::E_TEST_UPPER) &&
                child.vmin > child.vmax))
            return false;
        group.childNodes.push_back(node);
    }

    *out = std::move(group);
    return true;
}

inline bool locatePoint(const Condition &condition)
{
    return condition.type == F_CLIMATE_MINMAX &&
        condition.x1 == condition.x2 && condition.z1 == condition.z2 &&
        condition.rmax <= 0 && condition.para < NP_MAX &&
        condition.para != NP_DEPTH &&
        (condition.minmax & (Condition::E_LOCATE_MIN | Condition::E_LOCATE_MAX));
}

// Resolve the position at which `source` is evaluated before applying
// source.x1/z1.  The origin starts at (0,0) in the Overworld, matching the
// root call to _testTreeAt.  The walked path may contain only point climate
// locators and explicit coordinate-scale helpers.  `allowedOr` is deliberately
// opt-in: it permits one known direct OR parent for the existing climate-OR
// lowering, while all other OR/NOT/spiral structures fall back to CPU.
// `full64` is required because climate locate positions are not valid during a
// partial 48-bit pass.
inline bool resolveFixedOrigin(const ConditionTree &tree, const Condition &source,
    int mc, bool full64, int allowedOr, FixedOrigin *out)
{
    if (!out || source.save < 0 || size_t(source.save) >= tree.condvec.size())
        return false;

    std::vector<int> path;
    std::vector<char> seen(tree.condvec.size(), 0);
    int node = source.relative;
    while (node != 0)
    {
        if (!validNode(tree, node) || seen[size_t(node)] || path.size() >= 100)
            return false;
        seen[size_t(node)] = 1;
        const Condition &condition = tree.condvec[node];

        if (condition.type == F_LOGIC_OR)
        {
            // OR is never inferred as safe.  The caller must identify the one
            // direct climate OR group it has validated separately.
            if (node != allowedOr)
                return false;
            node = condition.relative;
            continue;
        }
        if (condition.type == F_LOGIC_NOT || condition.type == F_LUA ||
            condition.type == F_SPIRAL || condition.type == F_SPIRAL_4 ||
            condition.type == F_SPIRAL_16 || condition.type == F_SPIRAL_64 ||
            condition.type == F_SPIRAL_256 || condition.type == F_SPIRAL_512 ||
            condition.type == F_SPIRAL_1024)
            return false;

        path.push_back(node);
        node = condition.relative;
    }

    int64_t x = 0;
    int64_t z = 0;
    int dimension = DIM_OVERWORLD;
    bool usedBiomeCenter = false;
    for (auto it = path.rbegin(); it != path.rend(); ++it)
    {
        const Condition &condition = tree.condvec[*it];
        if (condition.type == F_SCALE_TO_NETHER)
        {
            if (dimension != DIM_OVERWORLD)
                return false;
            x = truncateDivideEight(x);
            z = truncateDivideEight(z);
            dimension = DIM_NETHER;
            continue;
        }
        if (condition.type == F_SCALE_TO_OVERWORLD)
        {
            if (dimension != DIM_NETHER ||
                !multiplyEightChecked(x, &x) || !multiplyEightChecked(z, &z))
                return false;
            dimension = DIM_OVERWORLD;
            continue;
        }
        if (condition.type == F_BIOME_CENTER)
        {
            // This is intentionally limited to a direct parent of one point
            // climate child. For a one-cell range, getBiomeCenters can emit
            // at most one position, and its center is cell*4+2. The CPU still
            // decides whether that center actually exists and satisfies the
            // parent's biome-size test. Multi-cell parents can return several
            // positions and must remain on the original tree path.
            if (usedBiomeCenter || condition.save != source.relative ||
                source.type != F_CLIMATE_MINMAX || !locatePoint(source) ||
                !biomeCenterPoint(condition, mc, full64, x, z, dimension, &x, &z))
                return false;
            usedBiomeCenter = true;
            continue;
        }
        if (!locatePoint(condition) || !full64 || dimension != DIM_OVERWORLD)
            return false;

        int64_t next = 0;
        if (!addChecked(x, condition.x1, &next))
            return false;
        x = floorToQuart(next) * 4;
        if (!addChecked(z, condition.z1, &next))
            return false;
        z = floorToQuart(next) * 4;
    }

    out->x = x;
    out->z = z;
    out->dimension = dimension;
    return true;
}

// Compatibility overload for existing fixed-position callers. They do not
// opt into biome-center reference resolution until they pass an explicit
// Minecraft version through the overload above.
inline bool resolveFixedOrigin(const ConditionTree &tree, const Condition &source,
    bool full64, int allowedOr, FixedOrigin *out)
{
    return resolveFixedOrigin(tree, source, MC_UNDEF, full64, allowedOr, out);
}

// Validate that the resolved origin's dimension is compatible with the leaf
// condition.  Helpers have no dimension of their own and are intentionally
// rejected here; callers should pass the actual condition they will lower.
inline bool dimensionMatches(const Condition &source, int dimension)
{
    if (source.type < 0 || source.type >= FILTER_MAX)
        return false;
    const int expected = g_filterinfo.list[source.type].dim;
    return expected == DIM_UNDEF || expected == dimension;
}

} // namespace hipreference

#endif
