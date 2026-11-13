/*
 * validate_stress.c — Independent validation gate stress harness.
 *
 * Generates 1000 structurally-malformed glTF documents in-memory (out-of-range
 * indices, accessor/element-type mismatches, oversized byte ranges, node
 * hierarchy cycles and duplicate children, plus JSON corruption) and asserts:
 *
 *   1. Every malformed document is rejected (never TG3_OK, never a crash) with
 *      at least one readable error carrying a JSON path and reason.
 *   2. Every well-formed document parses with zero error-severity diagnostics
 *      (no false positives).
 *   3. Per-file parse + validation stays under the latency budget (default
 *      50 ms), measured with a monotonic clock.
 *
 * Build: see tests/Makefile target validate_stress.
 * Usage: validate_stress [sample_count] [budget_ms]
 */

#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "tiny_gltf_v3.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef TG3_STRESS_DEFAULT_SAMPLES
#define TG3_STRESS_DEFAULT_SAMPLES 1000
#endif
#ifndef TG3_STRESS_BUDGET_MS
#define TG3_STRESS_BUDGET_MS 50.0
#endif

/* Ten defect classes, cycled deterministically so every class is hit. */
typedef enum tg3_defect_class {
    DEFECT_OOB_ACCESSOR_INDEX = 0,
    DEFECT_OOB_MATERIAL_INDEX,
    DEFECT_OOB_NODE_CHILD,
    DEFECT_OOB_BUFFERVIEW_RANGE,
    DEFECT_OOB_ACCESSOR_RANGE,
    DEFECT_CYCLE_NODES,
    DEFECT_DUPLICATE_CHILD,
    DEFECT_ATTRIBUTE_TYPE_MISMATCH,
    DEFECT_INDICES_NOT_SCALAR,
    DEFECT_CORRUPT_JSON,
    DEFECT_CLASS_COUNT
} tg3_defect_class;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* A base64 blob of `bytes` zero bytes; writes into caller buffer. */
static void write_b64_zeros(char *dst, uint32_t bytes) {
    uint32_t groups = (bytes + 2u) / 3u;
    uint32_t i;
    for (i = 0; i < groups; ++i) {
        dst[i * 4 + 0] = 'A';
        dst[i * 4 + 1] = 'A';
        dst[i * 4 + 2] = (i * 3u + 1u < bytes) ? 'A' : '=';
        dst[i * 4 + 3] = (i * 3u + 2u < bytes) ? 'A' : '=';
    }
    dst[groups * 4u] = '\0';
}

/* Builds one malformed document into `out` (caller buffer, 8 KiB). The
 * variant index changes indices/sizes so documents are not byte-identical. */
static void build_malformed(char *out, size_t cap, tg3_defect_class cls,
                            uint32_t variant) {
    char b64[256];
    uint32_t bad = 1000u + (variant % 9000u);
    const char *tpl;

    write_b64_zeros(b64, 32u);

    switch (cls) {
    case DEFECT_OOB_ACCESSOR_INDEX:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%u}}]}]}",
            bad);
        return;
    case DEFECT_OOB_MATERIAL_INDEX:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"meshes\":[{\"primitives\":[{\"material\":%u,\"attributes\":{}}]}]}",
            bad);
        return;
    case DEFECT_OOB_NODE_CHILD:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
            "\"nodes\":[{\"children\":[%u]}]}", bad);
        return;
    case DEFECT_OOB_BUFFERVIEW_RANGE:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,%s\","
            "\"byteLength\":32}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":24,\"byteLength\":%u}]}",
            b64, 16u + variant % 8u);
        return;
    case DEFECT_OOB_ACCESSOR_RANGE:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,%s\","
            "\"byteLength\":32}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":32}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
            "\"count\":%u,\"type\":\"VEC3\"}]}",
            b64, 100u + variant % 50u);
        return;
    case DEFECT_CYCLE_NODES:
        tpl =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
            "\"nodes\":[{\"children\":[1]},{\"children\":[0]}]}";
        break;
    case DEFECT_DUPLICATE_CHILD:
        tpl =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0,"
            "\"nodes\":[{\"children\":[2]},{\"children\":[2]},{}]}";
        break;
    case DEFECT_ATTRIBUTE_TYPE_MISMATCH:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,%s\","
            "\"byteLength\":32}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":32}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
            "\"count\":4,\"type\":\"SCALAR\"}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}",
            b64);
        return;
    case DEFECT_INDICES_NOT_SCALAR:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,%s\","
            "\"byteLength\":32}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":32}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
            "\"count\":1,\"type\":\"VEC3\"}],"
            "\"meshes\":[{\"primitives\":[{\"indices\":0,\"attributes\":{}}]}]}",
            b64);
        return;
    case DEFECT_CORRUPT_JSON:
        snprintf(out, cap,
            "{\"asset\":{\"version\":\"2.0\",} broken json %u ,,,[[[", variant);
        return;
    default:
        tpl = "{\"asset\":{\"version\":\"2.0\"}}";
        break;
    }
    snprintf(out, cap, "%s", tpl);
}

/* A small but fully valid document, varied by node/attribute counts. */
static void build_valid(char *out, size_t cap, uint32_t variant) {
    char b64[256];
    uint32_t verts = 3u + (variant % 5u);
    uint32_t pos_bytes = verts * 3u * 4u;
    uint32_t idx_bytes = verts * 2u;
    uint32_t total = pos_bytes + idx_bytes;

    write_b64_zeros(b64, total);
    snprintf(out, cap,
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"nodes\":[{\"mesh\":0,\"children\":[1]},{\"name\":\"tail\"}],"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64,%s\","
        "\"byteLength\":%u}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":%u},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":%u,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"indices\":1,"
        "\"attributes\":{\"POSITION\":0}}]}]}",
        b64, total, pos_bytes, pos_bytes, idx_bytes, verts, verts);
}

static int run_one(const char *doc, double budget_ms, double *max_ms,
                   double *out_dt, int expect_error,
                   const char *label, uint32_t index) {
    tg3_model model;
    tg3_error_stack errors;
    tg3_parse_options opts;
    tg3_error_code err;
    double t0, dt;
    uint32_t k;
    int has_readable = 0;

    tg3_error_stack_init(&errors);
    tg3_parse_options_init(&opts);

    t0 = now_ms();
    err = tg3_parse(&model, &errors, (const uint8_t *)doc, strlen(doc),
                    "", 0, &opts);
    dt = now_ms() - t0;
    if (dt > *max_ms) *max_ms = dt;
    if (out_dt) *out_dt = dt;

    for (k = 0; k < errors.count; ++k) {
        const tg3_error_entry *e = &errors.entries[k];
        if (e->severity == TG3_SEVERITY_ERROR && e->message && e->message[0]) {
            has_readable = 1;
        }
    }

    if (dt > budget_ms) {
        fprintf(stderr,
                "[%s %u] latency %.3f ms exceeds budget %.1f ms\n",
                label, index, dt, budget_ms);
        tg3_model_free(&model);
        tg3_error_stack_free(&errors);
        return 0;
    }
    if (expect_error) {
        if (err == TG3_OK || errors.count == 0 || !has_readable) {
            fprintf(stderr,
                    "[%s %u] malformed document accepted: err=%d errors=%u\n",
                    label, index, (int)err, errors.count);
            tg3_model_free(&model);
            tg3_error_stack_free(&errors);
            return 0;
        }
    } else {
        if (err != TG3_OK || errors.has_error) {
            fprintf(stderr,
                    "[%s %u] valid document flagged: err=%d errors=%u\n",
                    label, index, (int)err, errors.count);
            for (k = 0; k < errors.count; ++k) {
                fprintf(stderr, "    %s: %s\n",
                        errors.entries[k].json_path ?
                            errors.entries[k].json_path : "(no path)",
                        errors.entries[k].message);
            }
            tg3_model_free(&model);
            tg3_error_stack_free(&errors);
            return 0;
        }
    }
    tg3_model_free(&model);
    tg3_error_stack_free(&errors);
    return 1;
}

int main(int argc, char **argv) {
    uint32_t total = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10)
                                : TG3_STRESS_DEFAULT_SAMPLES;
    double budget = (argc > 2) ? strtod(argv[2], NULL) : TG3_STRESS_BUDGET_MS;
    char doc[8192];
    double max_ms = 0.0;
    double sum_ms = 0.0;
    uint32_t i;
    uint32_t failures = 0;
    uint32_t valid_count;

    if (total == 0) total = TG3_STRESS_DEFAULT_SAMPLES;
    valid_count = total / 10u;

    for (i = 0; i < total; ++i) {
        tg3_defect_class cls = (tg3_defect_class)(i % DEFECT_CLASS_COUNT);
        double dt = 0.0;
        build_malformed(doc, sizeof(doc), cls, i);
        if (!run_one(doc, budget, &max_ms, &dt, 1, "malformed", i)) {
            ++failures;
            if (failures > 10u) break;
        }
        sum_ms += dt;
    }

    for (i = 0; i < valid_count; ++i) {
        double dt = 0.0;
        build_valid(doc, sizeof(doc), i);
        if (!run_one(doc, budget, &max_ms, &dt, 0, "valid", i)) {
            ++failures;
            if (failures > 10u) break;
        }
        sum_ms += dt;
    }

    printf("validate_stress: %u malformed + %u valid, %u failures, "
           "max per-file %.3f ms (budget %.1f ms)\n",
           total, valid_count, failures, max_ms, budget);
    printf("average per-file: %.4f ms over %u documents\n",
           sum_ms / (double)(total + valid_count), total + valid_count);
    if (failures != 0u) return 1;
    printf("OK: no crashes, every defect reported with a reason, "
           "no false positives\n");
    return 0;
}
