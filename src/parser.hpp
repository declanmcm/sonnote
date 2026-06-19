#include <cstdint>
#include <cmath>

struct Operand;

struct ArithNode {
  char type;
  char operation;
  Operand* operands;
  int val;
  ArithNode* pronumPowers[4];

  ArithNode(char t) : type(t) {
    operands = nullptr;
    operation = '~';
    val = 0;
    pronumPowers[0] = nullptr;
    pronumPowers[1] = nullptr;
    pronumPowers[2] = nullptr;
    pronumPowers[3] = nullptr;
  }
};

struct Operand {
  char operation;
  ArithNode* node;
  Operand* next;

  Operand(ArithNode* n, char op) : node(n), operation(op) {
    next = nullptr;
  }
};

int getNum(char*& str) {
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
    if (*str == '^') {
      str++;
      if (*str == '(') {
        str++;
        toReturn->pronumPowers[index] = parseE(str);
        str++;
        printf("next Char: %c\n", *str);
      }
    } else {
      char *one = (char*)malloc(1);
      one[0] = '1';
      toReturn->pronumPowers[index] = parseE(one);
      free(one - 1);
    }
  }
  //printf("pronumPowers: %d, %d, %d, %d\n", toReturn->pronumPowers[0], toReturn->pronumPowers[1], toReturn->pronumPowers[2], toReturn->pronumPowers[3]);
  return toReturn;
}

ArithNode* parseF(char*& str) {
  printf("parsing F\n");
  ArithNode* toReturn = new ArithNode('F');
  if (*str == '-') {
    str++;
    toReturn->operands = new Operand(parseF(str), '-');
  } else if (*str == '(') {
    str++;
    toReturn->operands = new Operand(parseE(str), '~');
    str++;
  } else {
    printf("parsing N\n");
    toReturn->operands = new Operand(parseN(str), '~');
  }
  return toReturn;
}

ArithNode* parseT(char*& str) {
  printf("parsing T\n");
  ArithNode* toReturn = new ArithNode('T');
  toReturn->operands = new Operand(parseF(str), '~');
  Operand* current = toReturn->operands;
  while (*str == '*' || *str == '/') {
    if (*str == '*') {
      str++;
      printf("hey *\n");
      current->next = new Operand(parseF(str), '*');
      current = current->next;
    } else if (*str == '/') {
      str++;
      current->next = new Operand(parseF(str), '/');
      current = current->next;
    }
  }
  return toReturn;
}

ArithNode* parseE(char*& str) {
  printf("parsing E\n");
  ArithNode* toReturn = new ArithNode('E');
  toReturn->operands = new Operand(parseT(str), '~');
  Operand* current = toReturn->operands;
  printf("next char: %c\n", *str);
  while (*str == '+' || *str == '-') {
    if (*str == '+') {
      printf("found plus\n");
      str++;
      current->next = new Operand(parseT(str), '+');
      current = current->next;
    } else if (*str == '-') {
      str++;
      current->next = new Operand(parseT(str), '-');
      current = current->next;
    }
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
      Operand* current = node->operands;
      float result = evaluate(current->node, a, b, c, d);
      current = current->next;
      while (current) {
        if (current->operation == '+')
          result += evaluate(current->node, a, b, c, d);
        else if (current->operation == '-')
          result -= evaluate(current->node, a, b, c, d);
        current = current->next;
      }
      return result;
    } else if (node->type == 'T') {
      printf("T");
      std::fflush(stdout);
      Operand* current = node->operands;
      float result = evaluate(current->node, a, b, c, d);
      current = current->next;
      while (current) {
        printf("got next\n");
        if (current->operation == '*')
          result = result * evaluate(current->node, a, b, c, d);
        else if (current->operation == '/')
          result = result / evaluate(current->node, a, b, c, d);
        current = current->next;
      }
      return result;
    } else if (node->type == 'F') {
      printf("F");
      std::fflush(stdout);
      if (node->operation == '~') {
        return evaluate(node->operands->node, a, b, c, d);
      } else if (node->operation == '-') {
        return -1 * evaluate(node->operands->node, a, b, c, d);
      }
    } else if (node->type == 'N') {
      printf("N");
      std::fflush(stdout);
      float pows[4];
      for (int i = 0; i < 4; i++) {
        if (node->pronumPowers[i])
          pows[i] = evaluate(node->pronumPowers[i], a, b, c, d);
        else
          pows[i] = 0;
      }
      float value = std::pow(a, pows[0]) * \
        std::pow(b, pows[1])  * std::pow(c, pows[2]) \
        * std::pow(d, pows[3]);
      if (node->val != 0)
        value = value * node->val;
      printf("value at N: %f\n", value);
      return value;
    }
    return 0;
  }
};
