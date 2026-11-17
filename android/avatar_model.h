#ifndef TSFP_AVATAR_MODEL_H
#define TSFP_AVATAR_MODEL_H

#include <stdint.h>

enum {
    TSFP_XBR_NODE_BYTES = 0xc8,
    TSFP_XBR_TYPE = 0x48,
    TSFP_XBR_MATRIX = 0x49,
    TSFP_XBR_PARENT = 0x4a,
    TSFP_XBR_CHILD = 0x4b,
    TSFP_XBR_NEXT = 0x4c,
    TSFP_XBR_NO_NODE = 0xff,
    TSFP_XBR_MAX_AVATAR_NODES = 96
};

enum { TSFP_AVATAR_LEFT, TSFP_AVATAR_RIGHT, TSFP_AVATAR_HANDS };
enum { TSFP_ARM_UPPER, TSFP_ARM_ELBOW_JOINT, TSFP_ARM_FOREARM,
       TSFP_ARM_WRIST_JOINT, TSFP_ARM_HAND, TSFP_ARM_MATRICES };

typedef struct {
    uint8_t matrix[TSFP_ARM_MATRICES];
} TsfpAvatarArm;

static inline const uint8_t *tsfp_avatar_node(const uint8_t *nodes, unsigned node)
{
    return nodes + node * TSFP_XBR_NODE_BYTES;
}

/* Type 3 joins neighboring meshes without adding a skeletal branch. A
 * broken child or sibling link returns an out-of-range child count. */
static inline unsigned tsfp_avatar_bone_children(const uint8_t *nodes, unsigned count, unsigned parent,
                                                 unsigned children[3])
{
    unsigned size = 0;
    unsigned node = tsfp_avatar_node(nodes, parent)[TSFP_XBR_CHILD];
    for (unsigned i = 0; node != TSFP_XBR_NO_NODE; ++i) {
        if (!node || node >= count || i >= count)
            return 4;
        const uint8_t *entry = tsfp_avatar_node(nodes, node);
        if (entry[TSFP_XBR_PARENT] != parent)
            return 4;
        if (entry[TSFP_XBR_TYPE] != 3) {
            if (size == 3)
                return 4;
            children[size++] = node;
        }
        node = entry[TSFP_XBR_NEXT];
    }
    return size;
}

static inline unsigned tsfp_avatar_spine_end(const uint8_t *nodes, unsigned count)
{
    if (count < 15 || count > TSFP_XBR_MAX_AVATAR_NODES ||
        nodes[TSFP_XBR_PARENT] != TSFP_XBR_NO_NODE)
        return 0;
    unsigned children[3];
    if (tsfp_avatar_bone_children(nodes, count, 0, children) != 3)
        return 0;

    unsigned spine = children[0];
    for (unsigned depth = 0; depth < count; ++depth) {
        const unsigned size = tsfp_avatar_bone_children(nodes, count, spine, children);
        /* The stock rig branches to the neck and both arms at the chest. */
        if (size == 3)
            return spine;
        if (size != 1)
            break;
        spine = children[0];
    }
    return 0;
}

static inline int tsfp_avatar_arm(const uint8_t *nodes, unsigned count, unsigned start,
                                  unsigned matrix_count, TsfpAvatarArm *arm)
{
    /* The drawable segments are the shoulder cap, upper arm, forearm and
     * hand. Type 1 is a helper and type 2 can add a skinning matrix at a
     * joint. Some rigs and LODs have no separate elbow or wrist matrix. */
    const unsigned segments[] = { TSFP_ARM_UPPER, TSFP_ARM_FOREARM, TSFP_ARM_HAND };
    unsigned segment = 0, connector = TSFP_XBR_NO_NODE;
    unsigned node = start;
    for (unsigned depth = 0; node != TSFP_XBR_NO_NODE; ++depth) {
        if (node >= count || depth >= count)
            return 0;
        const uint8_t *entry = tsfp_avatar_node(nodes, node);
        const unsigned type = entry[TSFP_XBR_TYPE], matrix = entry[TSFP_XBR_MATRIX];
        if (type == 0) {
            if (segment > 3 || matrix >= matrix_count)
                return 0;
            if (segment) {
                const unsigned slot = segments[segment - 1];
                arm->matrix[slot] = matrix;
                if (segment >= 2)
                    arm->matrix[slot - 1] = connector;
            }
            ++segment;
            connector = TSFP_XBR_NO_NODE;
        } else if (type == 2 && (segment == 2 || segment == 3)) {
            if (matrix >= matrix_count || connector != TSFP_XBR_NO_NODE)
                return 0;
            connector = matrix;
        } else if (type != 1 && type != 2) {
            return 0;
        }
        unsigned children[3];
        const unsigned size = tsfp_avatar_bone_children(nodes, count, node, children);
        if (size > 1)
            return 0;
        node = size ? children[0] : TSFP_XBR_NO_NODE;
    }
    return segment == 4;
}

static inline unsigned tsfp_avatar_arm_rig(const uint8_t *nodes, unsigned count, unsigned chest,
                                           unsigned matrix_count, TsfpAvatarArm arms[TSFP_AVATAR_HANDS])
{
    unsigned branches[3];
    if (chest >= count || !matrix_count || matrix_count > TSFP_XBR_MAX_AVATAR_NODES ||
        tsfp_avatar_bone_children(nodes, count, chest, branches) != 3)
        return 0;

    unsigned head = 0, found = 0;
    /* The caller resolves the sides from the shoulders in the draw palette. */
    const unsigned sides[] = { TSFP_AVATAR_RIGHT, TSFP_AVATAR_LEFT };
    for (unsigned i = 0; i < 3; ++i) {
        TsfpAvatarArm arm;
        if (tsfp_avatar_arm(nodes, count, branches[i], matrix_count, &arm)) {
            if (found == TSFP_AVATAR_HANDS)
                return 0;
            arms[sides[found++]] = arm;
        } else {
            head = branches[i];
        }
    }
    if (found != TSFP_AVATAR_HANDS)
        return 0;

    /* Each palette entry must be saved and restored exactly once. */
    uint8_t used[TSFP_XBR_MAX_AVATAR_NODES] = {0};
    for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
        for (unsigned joint = 0; joint < TSFP_ARM_MATRICES; ++joint) {
            const unsigned matrix = arms[hand].matrix[joint];
            if (matrix != TSFP_XBR_NO_NODE && used[matrix]++)
                return 0;
        }
    }
    return head;
}

static inline unsigned tsfp_avatar_pose_matrix(const TsfpAvatarArm *arm, unsigned joint)
{
    const unsigned matrix = arm->matrix[joint];
    /* Without a connector, the next segment starts at the same joint. */
    return matrix != TSFP_XBR_NO_NODE ? matrix : arm->matrix[joint + 1];
}

static inline int tsfp_avatar_hide_head(const uint8_t *nodes, unsigned count,
                                        unsigned head, unsigned node)
{
    for (unsigned depth = 0; node < count && depth < count; ++depth) {
        if (node == head)
            return 1;
        node = tsfp_avatar_node(nodes, node)[TSFP_XBR_PARENT];
    }
    return 0;
}

#endif
