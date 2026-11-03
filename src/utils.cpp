#include "utils.hpp"
#include <cstdio>

int getInt(char*& str, bool& noVal) {
  float val = 0;
  while (*str >= 48 && *str <= 57) {
    noVal = false;
    val = val * 10;
    val += *str - 48;
    str++;
  }
  return val;
}

Ratio simplifyR(Ratio ratio) {
  uint32_t gcdR = std::gcd(ratio.num, ratio.den);
  Ratio a{0,0};
  if (gcdR != 0) {
    a.num = ratio.num / gcdR;
    a.den = ratio.den / gcdR;
    return a;
  } else {
    return ratio;
  }
}

Ratio multiplyR(Ratio a, Ratio b) {
  Ratio c{0,0};
  c.num = a.num * b.num;
  c.den = a.den * b.den;
  return simplifyR(c);
}

Ratio multiplyR_NS(Ratio a, Ratio b) {
  Ratio c{0,0};
  c.num = a.num * b.num;
  c.den = a.den * b.den;
  return c;
}

Ratio divideR(Ratio a, Ratio b) {
  Ratio c{0,0};
  c.num = a.num * b.den;
  c.den = a.den * b.num;
  return simplifyR(c);
}

Ratio invertR(Ratio a) {
  uint32_t num = a.num;
  a.num = a.den;
  a.den = num;
  return a;
}

Ratio addR(Ratio a, Ratio b) {
  uint32_t aden = a.den;
  a = multiplyR_NS(a, {b.den, b.den});
  b = multiplyR_NS(b, {aden, aden});
  Ratio c = {a.num + b.num, a.den};
  return simplifyR(c);
}

// Assumes a is larger than b
Ratio subtractR(Ratio a, Ratio b) {
  uint32_t aden = a.den;
  a = multiplyR_NS(a, {b.den, b.den});
  b = multiplyR_NS(b, {aden, aden});
  Ratio c = {a.num - b.num, a.den};
  return simplifyR(c);
}
