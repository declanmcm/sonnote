#ifndef SNUTILS_H
#define SNUTILS_H
#include <numeric>
#include <cstdint>

struct Vec2 {
    float x;
    float y;
};

struct Ratio {
  uint32_t num;
  uint32_t den;

  float eval() {
    return (float)num / (float)den;
  }
};

int getInt(char*& str, bool& noVal);
Ratio simplifyR(Ratio ratio);
Ratio multiplyR(Ratio a, Ratio b);
Ratio divideR(Ratio a, Ratio b);
Ratio invertR(Ratio a);
Ratio addR(Ratio a, Ratio b);
Ratio subtractR(Ratio a, Ratio b);

#endif
