/* A YAML scalar read as a value (rpcs3/util/yaml_value.c), as the core's
 * settings, patches and lists read theirs.
 *
 * - Booleans: true/false, yes/no, on/off, y/n in any case, nothing else.
 * - Integers: decimal or 0x hex, a sign, all of it, up to 64 bits; empty,
 *   a sign or 0x alone, other bytes, spaces and overflow refused.
 * - Doubles: strtod's numbers, all of it; YAML's three spellings each of
 *   infinity (with a sign) and NaN, and no others; a number longer than
 *   the stack copy read too. */

#include "../../util/yaml_value.h"
#include "test_common.h"

#include <math.h>

static int is_bool(const char *s, int want)
{
   int v = -1;
   return yaml_value_bool(s, strlen(s), &v) && v == want;
}

static int is_int(const char *s, uint64_t want, int want_neg)
{
   uint64_t m = 0;
   int neg = 0;
   return yaml_value_int(s, strlen(s), &m, &neg) && m == want && neg == want_neg;
}

static int not_int(const char *s)
{
   uint64_t m;
   int neg;
   return !yaml_value_int(s, strlen(s), &m, &neg);
}

static int is_f64(const char *s, double want)
{
   double v = 0;
   return yaml_value_f64(s, strlen(s), &v) && v == want;
}

static int not_f64(const char *s)
{
   double v;
   return !yaml_value_f64(s, strlen(s), &v);
}

int main(void)
{
   int v;
   double d;
   char longnum[200];

   TEST_CHECK(is_bool("true", 1) && is_bool("TRUE", 1) && is_bool("Yes", 1) && is_bool("on", 1) && is_bool("Y", 1));
   TEST_CHECK(is_bool("false", 0) && is_bool("No", 0) && is_bool("OFF", 0) && is_bool("n", 0));
   TEST_CHECK(!yaml_value_bool("", 0, &v) && !yaml_value_bool("tru", 3, &v) && !yaml_value_bool("truee", 5, &v) && !yaml_value_bool("1", 1, &v));
   /* Only the bytes given: "yes" inside a longer buffer */
   TEST_CHECK(yaml_value_bool("yesterday", 3, &v) && v == 1);

   TEST_CHECK(is_int("0", 0, 0) && is_int("-0", 0, 1) && is_int("+5", 5, 0) && is_int("123", 123, 0));
   TEST_CHECK(is_int("0x10", 16, 0) && is_int("0X1f", 31, 0) && is_int("0xFFFFFFFFFFFFFFFF", 0xFFFFFFFFFFFFFFFFull, 0) && is_int("0x000000000000000001", 1, 0));
   TEST_CHECK(is_int("-0x8000000000000000", 0x8000000000000000ull, 1));
   TEST_CHECK(is_int("18446744073709551615", 18446744073709551615ull, 0));
   TEST_CHECK(not_int("18446744073709551616") && not_int("99999999999999999999") && not_int("0x10000000000000000"));
   TEST_CHECK(not_int("") && not_int("-") && not_int("+") && not_int("0x") && not_int("12a") && not_int("0x1g") && not_int(" 1") && not_int("1 ") && not_int("1.5"));
   /* "0x" with nothing after the x is not hex: "0x" alone is refused,
    * as from_chars refused it */
   TEST_CHECK(not_int("-0x"));

   TEST_CHECK(is_f64("1.5", 1.5) && is_f64("-2e3", -2000) && is_f64("0", 0) && is_f64("1e-5", 1e-5) && is_f64("42", 42));
   TEST_CHECK(is_f64(".inf", HUGE_VAL) && is_f64(".Inf", HUGE_VAL) && is_f64(".INF", HUGE_VAL) && is_f64("+.inf", HUGE_VAL) && is_f64("+.INF", HUGE_VAL));
   TEST_CHECK(is_f64("-.inf", -HUGE_VAL) && is_f64("-.Inf", -HUGE_VAL) && is_f64("-.INF", -HUGE_VAL));
   TEST_CHECK(yaml_value_f64(".nan", 4, &d) && d != d && yaml_value_f64(".NaN", 4, &d) && yaml_value_f64(".NAN", 4, &d));
   TEST_CHECK(not_f64("") && not_f64("abc") && not_f64("1.5x") && not_f64(".iNf") && not_f64(".nAn") && not_f64("1 "));
   /* Only the bytes given */
   TEST_CHECK(yaml_value_f64("2.5junk", 3, &d) && d == 2.5);
   /* Longer than the copy on the stack */
   memset(longnum, '0', sizeof(longnum));
   longnum[0] = '1';
   longnum[1] = '.';
   longnum[150] = '5';
   TEST_CHECK(yaml_value_f64(longnum, 151, &d) && d == 1.0);

   printf("test_yaml_value: %s\n", test_failures ? "FAIL" : "ok");
   return test_failures ? 1 : 0;
}
