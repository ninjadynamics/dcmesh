/*
 * dcmesh.h — DCMesh binary format definition
 *
 * Shared between the PC-side offline converter (glb_to_dcmesh) and the
 * Dreamcast runtime loader. Defines the .dcmesh file format, vertex
 * layout, and strip structures.
 *
 * The format is designed for zero-overhead loading on Dreamcast:
 *   - Vertex layout matches GLdc's Vertex struct and PVR hardware
 *   - Strips are pre-computed offline via meshoptimizer
 *   - No index buffers needed at runtime
 *   - Pre-expanded vertices for direct array submission
 */

#ifndef DCMESH_H
#define DCMESH_H

#include <stdint.h>

/* -------------------------------------------------------------------
 * File format magic and version
 * ---------------------------------------------------------------- */
#define DCMESH_MAGIC      0x4D434431  /* "DCM1" in little-endian */
#define DCMESH_VERSION    1

/* -------------------------------------------------------------------
 * On-disk vertex format — 24 bytes, matches rlgl batcher format
 *
 * Position: 3 x float (12 bytes)
 * Texcoord: 2 x float (8 bytes)
 * Color:    BGRA packed uint32_t (4 bytes)
 *
 * Note: This is the MODEL-SPACE vertex. Transform happens at runtime.
 * ---------------------------------------------------------------- */
typedef struct {
    float x, y, z;          /* 12 bytes — model-space position */
    float u, v;             /*  8 bytes — texture coordinate */
    uint32_t color;         /*  4 bytes — packed BGRA color */
} DCVertex;                 /* 24 bytes total */

/* -------------------------------------------------------------------
 * Strip descriptor — defines a contiguous run of strip vertices
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t first_vertex;  /* Index of first vertex in the mesh vertex array */
    uint32_t vertex_count;  /* Number of vertices in this strip */
} DCStrip;

/* -------------------------------------------------------------------
 * Per-submesh header — one material = one submesh
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t material_index;    /* Index into the model's material array */
    uint32_t vertex_count;      /* Total vertices in this submesh */
    uint32_t strip_count;       /* Number of triangle strips */
    uint32_t is_opaque;         /* 1 = opaque (Patch E eligible), 0 = translucent */
    /* Followed in file by:
     *   DCVertex vertices[vertex_count]
     *   DCStrip  strips[strip_count]
     */
} DCSubmeshHeader;

/* -------------------------------------------------------------------
 * File header
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t magic;             /* DCMESH_MAGIC */
    uint32_t version;           /* DCMESH_VERSION */
    uint32_t submesh_count;     /* Number of submeshes in the file */
    uint32_t total_vertices;    /* Total vertices across all submeshes */
    uint32_t total_strips;      /* Total strips across all submeshes */
    uint32_t reserved[3];       /* Future use, must be zero */
} DCMeshFileHeader;

/* -------------------------------------------------------------------
 * Runtime submesh — loaded into memory
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t material_index;
    uint32_t is_opaque;
    uint32_t vertex_count;
    uint32_t strip_count;
    DCVertex *vertices;         /* Allocated vertex array */
    DCStrip  *strips;           /* Allocated strip array */
} DCSubmesh;

/* -------------------------------------------------------------------
 * Runtime mesh — the complete loaded .dcmesh
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t submesh_count;
    DCSubmesh *submeshes;       /* Allocated submesh array */
} DCMeshData;

/* -------------------------------------------------------------------
 * Registry handle — used to associate raylib Mesh with DCMeshData
 *
 * On Dreamcast GL 1.1, Mesh.vaoId is unused (VAOs are GL 3.3+).
 * We repurpose it as an index into a global registry:
 *   0 = no DCMesh data (standard GLdc path)
 *   >0 = registry index (1-based, so actual index = vaoId - 1)
 * ---------------------------------------------------------------- */
#define DCMESH_REGISTRY_MAGIC_BASE  0xDC000000
#define DCMESH_IS_REGISTRY_ID(id)   (((id) & 0xFF000000) == DCMESH_REGISTRY_MAGIC_BASE)
#define DCMESH_REGISTRY_INDEX(id)   ((id) & 0x00FFFFFF)
#define DCMESH_MAKE_ID(idx)         (DCMESH_REGISTRY_MAGIC_BASE | ((idx) & 0x00FFFFFF))

#endif /* DCMESH_H */
