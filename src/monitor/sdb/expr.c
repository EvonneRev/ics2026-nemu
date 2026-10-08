/***************************************************************************************
* Copyright (c) 2014-2024 Zihao Yu, Nanjing University
*
* NEMU is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/
#include <memory/vaddr.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <isa.h>

/* We use the POSIX regex functions to process regular expressions.
 * Type 'man regex' for more information about POSIX regex functions.
 */
#include <regex.h>

enum {
  TK_NOTYPE = 256, TK_EQ,

  /* TODO: Add more token types */
  TK_DEC,
  TK_HEX,
  TK_REG,
  TK_NEQ,
  TK_AND,

};

static struct rule {
  const char *regex;
  int token_type;
} rules[] = {

  /* TODO: Add more rules.
   * Pay attention to the precedence level of different rules.
   */

  {" +", TK_NOTYPE},    // spaces
  {"\\+", '+'},         // plus
  {"-", '-'},
  {"\\*", '*'},
  {"/", '/'},
  {"\\(", '('},
  {"\\)", ')'},
  {"==", TK_EQ},        // equal
  {"!=", TK_NEQ},
  {"&&", TK_AND},
  {"0[xX][0-9a-fA-F]+", TK_HEX},
  {"[0-9]+", TK_DEC},
  {"\\$[a-zA-Z][a-zA-Z0-9]*", TK_REG},
};

#define NR_REGEX ARRLEN(rules)

static regex_t re[NR_REGEX] = {};

/* Rules are used for many times.
 * Therefore we compile them only once before any usage.
 */
void init_regex() {
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i ++) {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
    if (ret != 0) {
      regerror(ret, &re[i], error_msg, 128);
      panic("regex compilation failed: %s\n%s", error_msg, rules[i].regex);
    }
  }
}

typedef struct token {
  int type;
  char str[32];
} Token;

static Token tokens[32] __attribute__((used)) = {};
static int nr_token __attribute__((used))  = 0;

static bool make_token(char *e) {
  int position = 0;
  int i;
  regmatch_t pmatch;

  nr_token = 0;

  while (e[position] != '\0') {
    for (i = 0; i < NR_REGEX; i++) {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 &&
          pmatch.rm_so == 0) {
        char *substr_start = e + position;
        int substr_len = pmatch.rm_eo;

        position += substr_len;

        if (rules[i].token_type == TK_NOTYPE) {
          break;
        }

        if (nr_token >= 32) {
          printf("Expression contains too many tokens\n");
          return false;
        }

        if (substr_len >= (int)sizeof(tokens[nr_token].str)) {
          printf("Token is too long at position %d\n",
                 position - substr_len);
          return false;
        }

        tokens[nr_token].type = rules[i].token_type;

        memcpy(tokens[nr_token].str, substr_start, substr_len);
        tokens[nr_token].str[substr_len] = '\0';

        nr_token++;
        break;
      }
    }

    if (i == NR_REGEX) {
      printf("No match at position %d\n%s\n%*.s^\n",
             position, e, position, "");
      return false;
    }
  }

  return true;
}

static int token_pos = 0;

static int binary_priority(int type) {
  switch (type) {
    case TK_AND:
      return 1;

    case TK_EQ:
    case TK_NEQ:
      return 2;

    case '+':
    case '-':
      return 3;

    case '*':
    case '/':
      return 4;

    default:
      return 0;
  }
}

static word_t parse_binary_expr(
    int min_priority, bool evaluate, bool *success);

static word_t parse_atom(bool evaluate, bool *success) {
  if (!*success || token_pos >= nr_token) {
    *success = false;
    return 0;
  }

  Token *token = &tokens[token_pos++];

  /*
   * 十进制和十六进制常量。
   *
   * 十进制必须使用 base 10，不能统一使用 base 0，
   * 否则 08 会被当成非法八进制数。
   */
  if (token->type == TK_DEC || token->type == TK_HEX) {
    char *end = NULL;
    int base = token->type == TK_HEX ? 16 : 10;

    errno = 0;
    unsigned long long raw = strtoull(token->str, &end, base);

    if (errno == ERANGE || end == NULL || *end != '\0') {
      *success = false;
      return 0;
    }

    word_t value = (word_t)raw;

    /*
     * 检查常量是否超过 word_t 能表示的范围。
     * 对 RV32 来说，4294967296 应被拒绝。
     */
    if ((unsigned long long)value != raw) {
      *success = false;
      return 0;
    }

    return evaluate ? value : 0;
  }

  /*
   * 寄存器。
   *
   * token->str 是 "$a0"，传给 isa_reg_str2val() 时去掉 '$'。
   */
  if (token->type == TK_REG) {
    if (!evaluate) {
      return 0;
    }

    bool reg_success = false;
    word_t value =
        isa_reg_str2val(token->str + 1, &reg_success);

    if (!reg_success) {
      printf("Unknown register: %s\n", token->str);
      *success = false;
      return 0;
    }

    return value;
  }

  /*
   * 括号表达式。
   */
  if (token->type == '(') {
    word_t value =
        parse_binary_expr(1, evaluate, success);

    if (!*success) {
      return 0;
    }

    if (token_pos >= nr_token ||
        tokens[token_pos].type != ')') {
      printf("Missing ')'\n");
      *success = false;
      return 0;
    }

    token_pos++;
    return value;
  }

  /*
   * 一元运算。
   *
   * 这里的 '*' 位于需要操作数的位置，所以表示解引用。
   * 二元乘法由 parse_binary_expr() 处理。
   */
  if (token->type == '+' ||
      token->type == '-' ||
      token->type == '*') {
    word_t value = parse_atom(evaluate, success);

    if (!*success || !evaluate) {
      return 0;
    }

    if (token->type == '+') {
      return value;
    }

    if (token->type == '-') {
      return (word_t)0 - value;
    }

    /*
     * PA/NEMU 表达式实验通常规定解引用读取4字节。
     */
    return vaddr_read((vaddr_t)value, 4);
  }

  printf("Unexpected token: %s\n", token->str);
  *success = false;
  return 0;
}

static word_t parse_binary_expr(
    int min_priority, bool evaluate, bool *success) {
  word_t left = parse_atom(evaluate, success);

  while (*success && token_pos < nr_token) {
    int op = tokens[token_pos].type;
    int priority = binary_priority(op);

    /*
     * 当前 token 不是二元运算符，或者优先级属于外层。
     * ')' 的 priority 也是 0，所以会在这里退出。
     */
    if (priority < min_priority) {
      break;
    }

    token_pos++;

    /*
     * 所有二元运算符都采用左结合。
     *
     * 例如：
     *   8 - 3 - 2
     * 应解释为：
     *   (8 - 3) - 2
     */
    bool evaluate_right =
        evaluate &&
        !(op == TK_AND && left == 0);

    word_t right = parse_binary_expr(
        priority + 1, evaluate_right, success);

    if (!*success) {
      return 0;
    }

    /*
     * && 短路时仍会解析右侧语法，但不真正读取寄存器或内存，
     * 也不执行右侧除法。
     */
    if (!evaluate) {
      left = 0;
      continue;
    }

    switch (op) {
      case '+':
        left = left + right;
        break;

      case '-':
        left = left - right;
        break;

      case '*':
        left = left * right;
        break;

      case '/':
        if (right == 0) {
          printf("Division by zero\n");
          *success = false;
          return 0;
        }

        left = left / right;
        break;

      case TK_EQ:
        left = (left == right);
        break;

      case TK_NEQ:
        left = (left != right);
        break;

      case TK_AND:
        left = (left != 0 && right != 0);
        break;

      default:
        *success = false;
        return 0;
    }
  }

  return left;
}

word_t expr(char *e, bool *success) {
  if (success == NULL) {
    return 0;
  }

  *success = false;

  if (e == NULL) {
    return 0;
  }

  if (!make_token(e)) {
    return 0;
  }

  if (nr_token == 0) {
    printf("Empty expression\n");
    return 0;
  }

  token_pos = 0;

  bool parse_success = true;
  word_t result =
      parse_binary_expr(1, true, &parse_success);

  /*
   * 如果没有消耗所有 token，说明存在多余的右括号、
   * 连续数字或省略运算符等错误。
   */
  if (token_pos != nr_token) {
    parse_success = false;

    if (token_pos < nr_token) {
      printf("Unexpected token: %s\n",
             tokens[token_pos].str);
    }
  }

  *success = parse_success;
  return parse_success ? result : 0;
}