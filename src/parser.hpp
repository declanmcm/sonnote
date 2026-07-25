#ifndef INCLUDE_PARSER_HPP
#define INCLUDE_PARSER_HPP

#include <cstdint>
#include <cmath>
#include <cstdio>

struct Operand;


struct ArithNode {
  char type;
  char operation;
  Operand* operands;
  float val;
  bool noVal;
  ArithNode* pronumPowers[4];

  ArithNode(char t) : type(t) {
    operands = nullptr;
    operation = '~';
    noVal = true;
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

float getNum(char*&, bool& noVal);
ArithNode* parseE(char*&);
ArithNode* parseT(char*&);
ArithNode* parseN(char*&);
ArithNode* parseF(char*&);

struct ArithTree {
  ArithNode* root;

  ArithTree() {
    root = nullptr;
  }

  ArithTree(char* str) {
    root = parseE(str);
  }

  float evaluate(float a, float b, float c, float d) {
    if (root)
      return evaluate(root, a, b, c, d);
    else
      return 0;
  }

  float evaluate(ArithNode* node, float a, float b, float c, float d) {
    if (node->type == 'E') {
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
      //printf("T");
      //std::fflush(stdout);
      Operand* current = node->operands;
      float result = evaluate(current->node, a, b, c, d);
      current = current->next;
      while (current) {
        //printf("got next\n");
        if (current->operation == '*')
          result = result * evaluate(current->node, a, b, c, d);
        else if (current->operation == '/')
          result = result / evaluate(current->node, a, b, c, d);
        current = current->next;
      }
      return result;
    } else if (node->type == 'F') {
      //printf("F");
      //std::fflush(stdout);
      if (node->operation == '~') {
        return evaluate(node->operands->node, a, b, c, d);
      } else if (node->operation == '-') {
        return -1 * evaluate(node->operands->node, a, b, c, d);
      }
    } else if (node->type == 'N') {
      //printf("N");
      //std::fflush(stdout);
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
      if (!node->noVal)
        value = value * node->val;
      //printf("value at N: %f\n", value);
      return value;
    }
    return 0;
  }
};

#endif
