#include <cstdint>
#include <cmath>

struct ArithNode {
  char type;
  char operation;
  ArithNode* left;
  ArithNode* right;
  int val;
  uint8_t pronumPowers[4];

  ArithNode(char t) : type(t) {
    left = nullptr;
    right = nullptr;
    operation = '~';
    val = 0;
    pronumPowers[0] = 0;
    pronumPowers[1] = 0;
    pronumPowers[2] = 0;
    pronumPowers[3] = 0;
  }
};

int getNum(char* str) {
  int val = 0;
  while (*str >= 48 && *str <= 57) {
    val = val * 10;
    val += *str - 48;
    str++;
  }
  return val;
}

ArithNode* parseE(char*&);

ArithNode* parseN(char*& str) {
  ArithNode* toReturn = new ArithNode('N');
  toReturn->val = getNum(str);
  while (*str == 'a' || *str == 'b' || *str == 'c' || *str == 'd') {
    int index = -1;
    if (*str == 'a')
      index = 0;
    else if (*str == 'b')
      index = 1;
    else if (*str == 'c')
      index = 2;
    else if (*str == 'd')
      index = 3;
    str++;
    printf("index: %d\n", index);
    if (*str == '^')
      toReturn->pronumPowers[index] = getNum(str);
    else
      toReturn->pronumPowers[index] = 1;
  }
  printf("pronumPowers: %d, %d, %d, %d\n", toReturn->pronumPowers[0], toReturn->pronumPowers[1], toReturn->pronumPowers[2], toReturn->pronumPowers[3]);
  return toReturn;
}

ArithNode* parseF(char*& str) {
  ArithNode* toReturn = new ArithNode('F');
  if (*str == '-') {
    str++;
    toReturn->operation = '-';
    toReturn->left = parseF(str);
  } else if (*str == '(') {
    str++;
    toReturn->left = parseE(str);
    str++;
  } else {
    printf("parsing N\n");
    toReturn->left = parseN(str);
  }
  return toReturn;
}

ArithNode* parseT(char*& str) {
  ArithNode* toReturn = new ArithNode('T');
  toReturn->left = parseF(str);
  if (*str == '*') {
    str++;
    printf("hey *\n");
    toReturn->operation = '*';
    toReturn->right = parseF(str);
  } else if (*str == '/') {
    str++;
    toReturn->operation = '/';
    toReturn->right = parseF(str);
  }
  return toReturn;
}

ArithNode* parseE(char*& str) {
  ArithNode* toReturn = new ArithNode('E');
  toReturn->left = parseT(str);
  if (*str == '+') {
    str++;
    toReturn->operation = '+';
    toReturn->right = parseT(str);
  } else if (*str == '-') {
    str++;
    toReturn->operation = '-';
    toReturn->right = parseT(str);
  }
  return toReturn;
}

struct ArithTree {
  ArithNode* root;

  ArithTree(char* str) {
    root = parseE(str);
  }

  float evaluate(float a, float b, float c, float d) {
    return evaluate(root, a, b, c, d);
  }

  float evaluate(ArithNode* node, float a, float b, float c, float d) {
    if (node->type == 'E') {
      printf("E");
      std::fflush(stdout);
      if (node->operation == '~') {
        return evaluate(node->left, a, b, c, d);
      } else if (node->operation == '+') {
        return evaluate(node->left, a, b, c, d) + evaluate(node->right, a, b, c, d);
      } else if (node->operation == '-') {
        return evaluate(node->left, a, b, c, d) - evaluate(node->right, a, b, c, d);
      }
    } else if (node->type == 'T') {
      printf("T");
      std::fflush(stdout);
      if (node->operation == '~') {
        return evaluate(node->left, a, b, c, d);
      } else if (node->operation == '*') {
        float value = evaluate(node->left, a, b, c, d) * evaluate(node->right, a, b, c, d);
        printf("value at T: %f\n", value);
        return value;
      } else if (node->operation == '/') {
        return evaluate(node->left, a, b, c, d) / evaluate(node->right, a, b, c, d);
      }
    } else if (node->type == 'F') {
      printf("F");
      std::fflush(stdout);
      if (node->operation == '~') {
        return evaluate(node->left, a, b, c, d);
      } else if (node->operation == '-') {
        return -1 * evaluate(node->left, a, b, c, d);
      }
    } else if (node->type == 'N') {
      printf("N");
      std::fflush(stdout);
      float value = std::pow(a, node->pronumPowers[0]) * \
        std::pow(b, node->pronumPowers[1])  * std::pow(c, node->pronumPowers[2]) \
        * std::pow(d, node->pronumPowers[3]);
      if (node->val != 0)
        value = value * node->val;
      printf("value at N: %f\n", value);
      return value;
    }
    return 0;
  }
};
