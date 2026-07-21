#ifndef XV6_RBTREE_H
#define XV6_RBTREE_H

#define RB_RED   0
#define RB_BLACK 1

struct rb_node {
  struct rb_node *parent;
  struct rb_node *left;
  struct rb_node *right;
  unsigned char color;
};

struct rb_root_cached {
  struct rb_node *root;
  struct rb_node *leftmost;
};

void rb_root_init(struct rb_root_cached *root);
void rb_node_init(struct rb_node *node);
int rb_node_empty(const struct rb_node *node);
void rb_link_node(struct rb_node *node, struct rb_node *parent,
                  struct rb_node **link);
void rb_insert_color_cached(struct rb_root_cached *root,
                            struct rb_node *node, int leftmost);
void rb_erase_cached(struct rb_root_cached *root, struct rb_node *node);
struct rb_node *rb_first(const struct rb_root_cached *root);
struct rb_node *rb_next(const struct rb_node *node);

#endif
