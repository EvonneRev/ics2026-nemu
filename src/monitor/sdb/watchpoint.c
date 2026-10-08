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

#include "sdb.h"

#define NR_WP 32

typedef struct watchpoint {
  int NO;
  struct watchpoint *next;

  /* TODO: Add more members if necessary */
  char expr[256];       // 保存监视的表达式
  word_t last_value;    // 上一次计算结果

} WP;

static WP wp_pool[NR_WP] = {};
static WP *head = NULL, *free_ = NULL;

void init_wp_pool() {
  int i;
  for (i = 0; i < NR_WP; i ++) {
    wp_pool[i].NO = i;
    wp_pool[i].next = (i == NR_WP - 1 ? NULL : &wp_pool[i + 1]);
  }

  head = NULL;
  free_ = wp_pool;
}

/* TODO: Implement the functionality of watchpoint */
WP *new_wp(char *e);
WP *new_wp(char *e) {
  if (free_ == NULL) {
    printf("No free watchpoint.\n");
    return NULL;
  }

  bool success = false;
  word_t value = expr(e, &success);

  if (!success) {
    printf("Bad expression.\n");
    return NULL;
  }

  WP *wp = free_;
  free_ = free_->next;

  strncpy(wp->expr, e, sizeof(wp->expr) - 1);
  wp->expr[sizeof(wp->expr) - 1] = '\0';
  wp->last_value = value;

  wp->next = head;
  head = wp;

  printf("Watchpoint %d: %s\n", wp->NO, wp->expr);
  return wp;
}

void free_wp(int no) {
  WP *prev = NULL;
  WP *cur = head;

  while (cur != NULL) {
    if (cur->NO == no) {
      if (prev == NULL)
        head = cur->next;
      else
        prev->next = cur->next;

      cur->next = free_;
      free_ = cur;

      printf("Watchpoint %d deleted.\n", no);
      return;
    }

    prev = cur;
    cur = cur->next;
  }

  printf("Watchpoint %d not found.\n", no);
}

bool scan_wp(void) {
  bool triggered = false;

  for (WP *wp = head; wp != NULL; wp = wp->next) {
    bool success = false;
    word_t new_value = expr(wp->expr, &success);

    if (!success) {
      printf("Failed to evaluate watchpoint %d.\n", wp->NO);
      continue;
    }

    if (new_value != wp->last_value) {
      printf("Watchpoint %d triggered: %s\n", wp->NO, wp->expr);
      printf("Old value = " FMT_WORD ", new value = " FMT_WORD "\n",
             wp->last_value, new_value);

      wp->last_value = new_value;
      triggered = true;
    }
  }

  return triggered;
}

void print_watchpoints(void) {
  for (WP *wp = head; wp != NULL; wp = wp->next) {
    printf("%d\t%s\t" FMT_WORD "\n",
           wp->NO, wp->expr, wp->last_value);
  }
}