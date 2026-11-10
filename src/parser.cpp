#include "parser.hpp"
#include <vector>

//int main(int argc, char** argv) {
  //ArithTree* tree = new ArithTree(argv[1]);
  //printf("Result: %f\n", tree->evaluate(1.0, 2.0, 3.0, 4.0));
//  std::vector<int> factors = getPrimeFactors(atoi(argv[1]));
  //for (int i = 0; i < factors.size(); i++) {
  //  printf("%d: %d\n", i, factors[i]);
  //}
//  return 0;
//}

float getNum(char*& str, bool& noVal) {
  float val = 0;
  bool before = true;
  int place = 0;
  while ((*str >= 48 && *str <= 57) || *str == '.') {
    noVal = false;
    if (*str == '.') {
      before = false;
      str++;
      continue;
    }
    if (before) {
      val = val * 10;
      val += *str - 48;
    } else {
      place--;
      val += (*str - 48) * std::pow(10, place);
    }
    str++;
  }
  return val;
}

ArithNode* parseE(char*&);

ArithNode* parseN(char*& str) {
  ArithNode* toReturn = new ArithNode('N');
  toReturn->val = getNum(str, toReturn->noVal);
  while (*str == 'a' || *str == 'b' || *str == 'c' || *str == 'd') {
    printf("found a letter\n");
    std::fflush(stdout);
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
  printf("toReturn->val: %f\n", toReturn->val);
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
  if (!toReturn->operands->node) {
    while (1) {
      printf("H");
      std::fflush(stdout);
    }
  }
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
