#include <stdio.h>
#include <stdlib.h>
#include "kernel/rbtree.h"

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define container_of(ptr, type, member) \
  ((type *)((char *)(ptr) - (unsigned long)&((type *)0)->member))

struct item {
  int key;
  int inserted;
  struct rb_node node;
};

static struct rb_root_cached tree;

static int
check_node(struct rb_node *node, int min, int max, int *valid)
{
  int left_height;
  int right_height;
  struct item *item;

  if(node == 0)
    return 1;

  item = container_of(node, struct item, node);
  if(item->key <= min || item->key >= max)
    *valid = 0;
  if(node->color == RB_RED &&
     ((node->left && node->left->color == RB_RED) ||
      (node->right && node->right->color == RB_RED)))
    *valid = 0;
  if(node->left && node->left->parent != node)
    *valid = 0;
  if(node->right && node->right->parent != node)
    *valid = 0;

  left_height = check_node(node->left, min, item->key, valid);
  right_height = check_node(node->right, item->key, max, valid);
  if(left_height != right_height)
    *valid = 0;
  return left_height + (node->color == RB_BLACK);
}

static void
verify_tree(int expected_count)
{
  struct rb_node *node;
  int valid = 1;
  int count = 0;
  int previous = -2147483647;

  if(tree.root && tree.root->color != RB_BLACK)
    valid = 0;
  check_node(tree.root, -2147483647, 2147483647, &valid);

  for(node = rb_first(&tree); node; node = rb_next(node)){
    struct item *item = container_of(node, struct item, node);
    if(item->key <= previous)
      valid = 0;
    previous = item->key;
    count++;
  }

  if(count != expected_count)
    valid = 0;
  if((expected_count == 0) != (tree.leftmost == 0))
    valid = 0;
  if(!valid){
    fprintf(stderr, "red-black invariant failure: expected=%d actual=%d\n",
            expected_count, count);
    exit(1);
  }
}

static void
insert_item(struct item *item)
{
  struct rb_node **link = &tree.root;
  struct rb_node *parent = 0;
  int leftmost = 1;

  while(*link){
    struct item *current = container_of(*link, struct item, node);
    parent = *link;
    if(item->key < current->key){
      link = &parent->left;
    } else {
      leftmost = 0;
      link = &parent->right;
    }
  }

  rb_link_node(&item->node, parent, link);
  rb_insert_color_cached(&tree, &item->node, leftmost);
  item->inserted = 1;
}

int
main(void)
{
  int insert_order[] = {30, 20, 40, 10, 25, 35, 50, 5, 15, 27,
                        1, 6, 13, 17, 26, 28, 45, 60, 55, 70};
  int erase_order[] = {1, 6, 5, 10, 15, 13, 17, 20, 25, 26,
                       27, 28, 30, 35, 40, 45, 50, 55, 60, 70};
  struct item items[ARRAY_SIZE(insert_order)];
  int count = 0;
  int i;

  rb_root_init(&tree);
  for(i = 0; i < ARRAY_SIZE(insert_order); i++){
    items[i].key = insert_order[i];
    items[i].inserted = 0;
    rb_node_init(&items[i].node);
    insert_item(&items[i]);
    count++;
    verify_tree(count);
  }

  for(i = 0; i < ARRAY_SIZE(erase_order); i++){
    int j;
    for(j = 0; j < ARRAY_SIZE(items); j++){
      if(items[j].key == erase_order[i]){
        rb_erase_cached(&tree, &items[j].node);
        items[j].inserted = 0;
        count--;
        verify_tree(count);
        break;
      }
    }
    if(j == ARRAY_SIZE(items))
      return 2;
  }

  puts("rbtree_test: OK");
  return 0;
}
