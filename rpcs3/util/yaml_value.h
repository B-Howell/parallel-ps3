#ifndef YAML_VALUE_H__
#define YAML_VALUE_H__

/* A YAML scalar's bytes read as a value, in place: nothing copied to the
 * heap (util/yaml.cpp, over ryaml). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* true/yes/on/y or false/no/off/n, in any case: 1 with *out set, or 0 */
int yaml_value_bool(const char *s, size_t len, int *out);

/* Decimal, or hexadecimal after 0x, all of it, an optional sign first:
 * 1 with its magnitude and sign, or 0 (empty, other bytes, or more than
 * 64 bits) */
int yaml_value_int(const char *s, size_t len, uint64_t *magnitude, int *negative);

/* A number as strtod reads it, all of it, or YAML's spellings of
 * infinity and NaN: 1 with *out set, or 0 */
int yaml_value_f64(const char *s, size_t len, double *out);

#ifdef __cplusplus
}
#endif

#endif
