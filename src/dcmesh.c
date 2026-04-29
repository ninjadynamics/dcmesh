/*
 * glb_to_dcmesh.c — Offline GLB to DCMesh converter
 *
 * Converts .glb (glTF binary) models into .dcmesh files optimized for
 * Dreamcast triangle strip rendering.
 *
 * Pipeline:
 *   1. Load .glb via cgltf
 *   2. Extract mesh data per primitive (material)
 *   3. Optimize vertex cache ordering via meshoptimizer
 *   4. Stripify into triangle strips
 *   5. Pre-expand strip vertices (de-index for direct array submission)
 *   6. Write .dcmesh binary file
 *
 * Build (on PC, not cross-compiled):
 *   cc -O2 -o glb_to_dcmesh glb_to_dcmesh.c \
 *      -I/path/to/meshoptimizer/extern \
 *      -L/path/to/meshoptimizer/build -lmeshoptimizer -lm
 *
 * Or with meshoptimizer sources compiled directly:
 *   cc -O2 -o glb_to_dcmesh glb_to_dcmesh.c \
 *      /path/to/meshoptimizer/src/stripifier.cpp \
 *      /path/to/meshoptimizer/src/vcacheoptimizer.cpp \
 *      /path/to/meshoptimizer/src/indexgenerator.cpp \
 *      -I/path/to/meshoptimizer/extern \
 *      -I/path/to/meshoptimizer/src -lstdc++ -lm
 *
 * Usage:
 *   glb_to_dcmesh input.glb [output.dcmesh]
 *
 * If output is not specified, writes to input.dcmesh (replacing .glb extension).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "meshoptimizer.h"
#include "dcmesh.h"

/* -------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */


static float* accessor_to_floats(const cgltf_accessor* acc, int components) {
    size_t count = acc->count;
    float* out = (float*)malloc(count * components * sizeof(float));
    if (!out) return NULL;
    for (size_t i = 0; i < count; i++) {
        cgltf_accessor_read_float(acc, i, &out[i * components], components);
    }
    return out;
}

static unsigned int* accessor_to_indices(const cgltf_accessor* acc) {
    size_t count = acc->count;
    unsigned int* out = (unsigned int*)malloc(count * sizeof(unsigned int));
    if (!out) return NULL;
    for (size_t i = 0; i < count; i++) {
        out[i] = (unsigned int)cgltf_accessor_read_index(acc, i);
    }
    return out;
}

/* Pack RGBA bytes into BGRA uint32 */
static uint32_t pack_color_bgra(float r, float g, float b, float a) {
    uint8_t br = (uint8_t)(r * 255.0f);
    uint8_t bg = (uint8_t)(g * 255.0f);
    uint8_t bb = (uint8_t)(b * 255.0f);
    uint8_t ba = (uint8_t)(a * 255.0f);
    return ((uint32_t)ba << 24) | ((uint32_t)br << 16) | ((uint32_t)bg << 8) | (uint32_t)bb;
}

/* -------------------------------------------------------------------
 * Process one glTF primitive into a DCSubmesh
 * ---------------------------------------------------------------- */
typedef struct {
    DCSubmeshHeader header;
    DCVertex* vertices;
    DCStrip* strips;
    uint16_t* vertex_map;       /* Maps strip vertex -> original vertex index */
} ProcessedSubmesh;

static int process_primitive(const cgltf_primitive* prim, int mat_index,
                            const float* node_xform, ProcessedSubmesh* out) {
    if (prim->type != cgltf_primitive_type_triangles) {
        fprintf(stderr, "  Skipping non-triangle primitive (type=%d)\n", prim->type);
        return 0;
    }
    if (!prim->indices) {
        fprintf(stderr, "  Skipping non-indexed primitive\n");
        return 0;
    }

    /* Find position, texcoord, color accessors */
    const cgltf_accessor* pos_acc = NULL;
    const cgltf_accessor* uv_acc = NULL;
    const cgltf_accessor* col_acc = NULL;

    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_position)
            pos_acc = prim->attributes[i].data;
        else if (prim->attributes[i].type == cgltf_attribute_type_texcoord && !uv_acc)
            uv_acc = prim->attributes[i].data;
        else if (prim->attributes[i].type == cgltf_attribute_type_color && !col_acc)
            col_acc = prim->attributes[i].data;
    }

    if (!pos_acc) {
        fprintf(stderr, "  Skipping primitive with no positions\n");
        return 0;
    }

    size_t vertex_count = pos_acc->count;
    size_t index_count = prim->indices->count;

    printf("  Primitive: %zu vertices, %zu indices (%zu triangles)\n",
           vertex_count, index_count, index_count / 3);

    /* Read attribute data */
    float* positions = accessor_to_floats(pos_acc, 3);
    float* texcoords = uv_acc ? accessor_to_floats(uv_acc, 2) : NULL;
    float* colors = col_acc ? accessor_to_floats(col_acc, 4) : NULL;
    unsigned int* indices = accessor_to_indices(prim->indices);

    /* Apply node world transform to positions (matches raylib's LoadModel behavior).
     * node_xform is a column-major 4x4 matrix from cgltf_node_transform_world(). */
    if (positions && node_xform) {
        for (size_t i = 0; i < vertex_count; i++) {
            float x = positions[i * 3 + 0];
            float y = positions[i * 3 + 1];
            float z = positions[i * 3 + 2];
            /* Column-major multiply: M * [x, y, z, 1] */
            positions[i * 3 + 0] = node_xform[0]*x + node_xform[4]*y + node_xform[8]*z  + node_xform[12];
            positions[i * 3 + 1] = node_xform[1]*x + node_xform[5]*y + node_xform[9]*z  + node_xform[13];
            positions[i * 3 + 2] = node_xform[2]*x + node_xform[6]*y + node_xform[10]*z + node_xform[14];
        }
    }

    if (!positions || !indices) {
        free(positions); free(texcoords); free(colors); free(indices);
        return 0;
    }

    /* Step 1: Optimize vertex cache ordering for strip generation */
    unsigned int* optimized = (unsigned int*)malloc(index_count * sizeof(unsigned int));
    meshopt_optimizeVertexCacheStrip(optimized, indices, index_count, vertex_count);
    free(indices);

    /* Step 2: Stripify — use ~0 as restart index, then split on it */
    size_t strip_bound = meshopt_stripifyBound(index_count);
    unsigned int* strip_indices = (unsigned int*)malloc(strip_bound * sizeof(unsigned int));
    size_t strip_index_count = meshopt_stripify(strip_indices, optimized,
                                                 index_count, vertex_count, ~0u);
    free(optimized);

    printf("  Stripified: %zu strip indices (from %zu triangle indices)\n",
           strip_index_count, index_count);

    /* Step 3: Split on restart indices to find individual strips */
    /* Count strips first */
    size_t num_strips = 0;
    size_t run_start = 0;
    for (size_t i = 0; i <= strip_index_count; i++) {
        if (i == strip_index_count || strip_indices[i] == ~0u) {
            size_t run_len = i - run_start;
            if (run_len >= 3) num_strips++;
            run_start = i + 1;
        }
    }

    if (num_strips == 0) {
        fprintf(stderr, "  Warning: no valid strips generated\n");
        free(positions); free(texcoords); free(colors); free(strip_indices);
        return 0;
    }

    /* Step 4: Pre-expand strip vertices (de-index for direct array submission) */
    /* Count total expanded vertices */
    size_t total_expanded = 0;
    run_start = 0;
    for (size_t i = 0; i <= strip_index_count; i++) {
        if (i == strip_index_count || strip_indices[i] == ~0u) {
            size_t run_len = i - run_start;
            if (run_len >= 3) total_expanded += run_len;
            run_start = i + 1;
        }
    }

    DCVertex* expanded = (DCVertex*)malloc(total_expanded * sizeof(DCVertex));
    DCStrip* strips = (DCStrip*)malloc(num_strips * sizeof(DCStrip));
    uint16_t* vertex_map = (uint16_t*)malloc(total_expanded * sizeof(uint16_t));

    /* Default color: opaque white */
    uint32_t default_color = pack_color_bgra(1.0f, 1.0f, 1.0f, 1.0f);

    size_t vert_offset = 0;
    size_t strip_idx = 0;
    run_start = 0;

    for (size_t i = 0; i <= strip_index_count; i++) {
        if (i == strip_index_count || strip_indices[i] == ~0u) {
            size_t run_len = i - run_start;
            if (run_len >= 3) {
                strips[strip_idx].first_vertex = (uint32_t)vert_offset;
                strips[strip_idx].vertex_count = (uint32_t)run_len;

                for (size_t j = run_start; j < i; j++) {
                    unsigned int vi = strip_indices[j];
                    DCVertex* dv = &expanded[vert_offset];
                    vertex_map[vert_offset] = (uint16_t)vi;
                    vert_offset++;

                    dv->x = positions[vi * 3 + 0];
                    dv->y = positions[vi * 3 + 1];
                    dv->z = positions[vi * 3 + 2];

                    if (texcoords) {
                        dv->u = texcoords[vi * 2 + 0];
                        dv->v = texcoords[vi * 2 + 1];
                    } else {
                        dv->u = 0.0f;
                        dv->v = 0.0f;
                    }

                    if (colors) {
                        dv->color = pack_color_bgra(
                            colors[vi * 4 + 0], colors[vi * 4 + 1],
                            colors[vi * 4 + 2], colors[vi * 4 + 3]);
                    } else {
                        dv->color = default_color;
                    }
                }
                strip_idx++;
            }
            run_start = i + 1;
        }
    }

    /* Determine opacity from material */
    int is_opaque = 1;
    if (prim->material) {
        if (prim->material->alpha_mode != cgltf_alpha_mode_opaque)
            is_opaque = 0;
    }

    /* Fill output */
    out->header.material_index = (uint32_t)mat_index;
    out->header.vertex_count = (uint32_t)total_expanded;
    out->header.strip_count = (uint32_t)num_strips;
    out->header.is_opaque = (uint32_t)is_opaque;
    out->vertices = expanded;
    out->strips = strips;
    out->vertex_map = vertex_map;

    float ratio = (float)total_expanded / (float)(index_count);
    printf("  Result: %zu strips, %zu expanded vertices (%.1f%% of triangle list)\n",
           num_strips, total_expanded, ratio * 100.0f);
    printf("  Avg strip length: %.1f vertices\n",
           (float)total_expanded / (float)num_strips);
    printf("  Opaque: %s\n", is_opaque ? "yes" : "no");

    free(positions);
    free(texcoords);
    free(colors);
    free(strip_indices);
    return 1;
}

/* -------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: glb_to_dcmesh input.glb [output.dcmesh]\n");
        return 1;
    }

    const char* input_path = argv[1];
    char output_path[512];

    if (argc >= 3) {
        strncpy(output_path, argv[2], sizeof(output_path) - 1);
    } else {
        strncpy(output_path, input_path, sizeof(output_path) - 1);
        char* dot = strrchr(output_path, '.');
        if (dot) strcpy(dot, ".dcmesh");
        else strcat(output_path, ".dcmesh");
    }

    printf("Converting: %s -> %s\n", input_path, output_path);

    /* Load and parse glTF */
    cgltf_options options = {0};
    cgltf_data* data = NULL;
    cgltf_result result = cgltf_parse_file(&options, input_path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Error: Failed to parse %s (error %d)\n", input_path, result);
        return 1;
    }

    result = cgltf_load_buffers(&options, data, input_path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Error: Failed to load buffers\n");
        cgltf_free(data);
        return 1;
    }

    printf("Loaded: %zu meshes, %zu materials\n", data->meshes_count, data->materials_count);

    /* Process all primitives across all meshes */
    size_t max_submeshes = 0;
    for (cgltf_size m = 0; m < data->meshes_count; m++)
        max_submeshes += data->meshes[m].primitives_count;

    ProcessedSubmesh* submeshes = (ProcessedSubmesh*)calloc(max_submeshes, sizeof(ProcessedSubmesh));
    size_t submesh_count = 0;
    uint32_t total_vertices = 0;
    uint32_t total_strips = 0;

    for (cgltf_size m = 0; m < data->meshes_count; m++) {
        cgltf_mesh* mesh = &data->meshes[m];
        printf("\nMesh %zu: \"%s\" (%zu primitives)\n", m,
               mesh->name ? mesh->name : "(unnamed)", mesh->primitives_count);

        /* Find the node that references this mesh and get its world transform.
         * This matches what raylib's LoadModel does — it applies node transforms
         * to vertex positions during loading. Without this, dcmesh vertices
         * would be in raw accessor space (wrong scale/rotation/position). */
        float node_xform[16];
        int has_xform = 0;
        for (cgltf_size ni = 0; ni < data->nodes_count; ni++) {
            if (data->nodes[ni].mesh == mesh) {
                cgltf_node_transform_world(&data->nodes[ni], node_xform);
                has_xform = 1;
                printf("  Node transform found (node \"%s\")\n",
                       data->nodes[ni].name ? data->nodes[ni].name : "(unnamed)");
                break;
            }
        }

        for (cgltf_size p = 0; p < mesh->primitives_count; p++) {
            cgltf_primitive* prim = &mesh->primitives[p];

            /* Find material index */
            int mat_idx = 0;
            if (prim->material) {
                for (cgltf_size mi = 0; mi < data->materials_count; mi++) {
                    if (prim->material == &data->materials[mi]) {
                        mat_idx = (int)mi;
                        break;
                    }
                }
            }

            if (process_primitive(prim, mat_idx, has_xform ? node_xform : NULL,
                                  &submeshes[submesh_count])) {
                total_vertices += submeshes[submesh_count].header.vertex_count;
                total_strips += submeshes[submesh_count].header.strip_count;
                submesh_count++;
            }
        }
    }

    if (submesh_count == 0) {
        fprintf(stderr, "Error: No processable primitives found\n");
        free(submeshes);
        cgltf_free(data);
        return 1;
    }

    /* Write .dcmesh file */
    FILE* fout = fopen(output_path, "wb");
    if (!fout) {
        fprintf(stderr, "Error: Cannot open %s for writing\n", output_path);
        /* cleanup omitted for brevity */
        return 1;
    }

    /* File header */
    DCMeshFileHeader fhdr = {0};
    fhdr.magic = DCMESH_MAGIC;
    fhdr.version = DCMESH_VERSION;
    fhdr.submesh_count = (uint32_t)submesh_count;
    fhdr.total_vertices = total_vertices;
    fhdr.total_strips = total_strips;
    fwrite(&fhdr, sizeof(fhdr), 1, fout);

    /* Write each submesh: header, vertices, strips */
    for (size_t i = 0; i < submesh_count; i++) {
        ProcessedSubmesh* sm = &submeshes[i];
        fwrite(&sm->header, sizeof(DCSubmeshHeader), 1, fout);
        fwrite(sm->vertices, sizeof(DCVertex), sm->header.vertex_count, fout);
        fwrite(sm->strips, sizeof(DCStrip), sm->header.strip_count, fout);
        fwrite(sm->vertex_map, sizeof(uint16_t), sm->header.vertex_count, fout);
    }

    fclose(fout);

    printf("\n=== Summary ===\n");
    printf("Submeshes:      %zu\n", submesh_count);
    printf("Total vertices: %u\n", total_vertices);
    printf("Total strips:   %u\n", total_strips);
    printf("File size:      %zu bytes\n",
           sizeof(DCMeshFileHeader)
           + submesh_count * sizeof(DCSubmeshHeader)
           + total_vertices * sizeof(DCVertex)
           + total_strips * sizeof(DCStrip)
           + total_vertices * sizeof(uint16_t));
    printf("Written to:     %s\n", output_path);

    /* Cleanup */
    for (size_t i = 0; i < submesh_count; i++) {
        free(submeshes[i].vertices);
        free(submeshes[i].strips);
        free(submeshes[i].vertex_map);
    }
    free(submeshes);
    cgltf_free(data);
    return 0;
}
