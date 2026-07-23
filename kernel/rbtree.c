#include "rbtree.h"

static int
rb_is_red(const struct rb_node *node)
{
  return node != 0 && node->color == RB_RED;
}

static int
rb_is_black(const struct rb_node *node)
{
  return node == 0 || node->color == RB_BLACK;
}

void
rb_root_init(struct rb_root_cached *root)
{
  root->root = 0;
  root->leftmost = 0;
}

void
rb_node_init(struct rb_node *node)
{
  node->parent = 0;
  node->left = 0;
  node->right = 0;
  node->color = RB_RED;
}

int
rb_node_empty(const struct rb_node *node)
{
  return node->parent == 0 && node->left == 0 && node->right == 0;
}

void
rb_link_node(struct rb_node *node, struct rb_node *parent,
             struct rb_node **link)
{
  node->parent = parent;
  node->left = 0;
  node->right = 0;
  node->color = RB_RED;
  *link = node;
}

static void
rb_rotate_left(struct rb_root_cached *root, struct rb_node *node)
{
  struct rb_node *right = node->right;
  struct rb_node *parent = node->parent;

  node->right = right->left;
  if(right->left)
    right->left->parent = node;

  right->left = node;
  node->parent = right;
  right->parent = parent;

  if(parent == 0)
    root->root = right;
  else if(parent->left == node)
    parent->left = right;
  else
    parent->right = right;
}

static void
rb_rotate_right(struct rb_root_cached *root, struct rb_node *node)
{
  struct rb_node *left = node->left;
  struct rb_node *parent = node->parent;

  node->left = left->right;
  if(left->right)
    left->right->parent = node;

  left->right = node;
  node->parent = left;
  left->parent = parent;

  if(parent == 0)
    root->root = left;
  else if(parent->left == node)
    parent->left = left;
  else
    parent->right = left;
}

void
rb_insert_color_cached(struct rb_root_cached *root,
                       struct rb_node *node, int leftmost)
{
  struct rb_node *parent;
  struct rb_node *grandparent;

  if(leftmost || root->leftmost == 0)
    root->leftmost = node;

  while((parent = node->parent) != 0 && rb_is_red(parent)){
    grandparent = parent->parent;
    if(parent == grandparent->left){
      struct rb_node *uncle = grandparent->right;
      if(rb_is_red(uncle)){
        parent->color = RB_BLACK;
        uncle->color = RB_BLACK;
        grandparent->color = RB_RED;
        node = grandparent;
        continue;
      }
      if(node == parent->right){
        node = parent;
        rb_rotate_left(root, node);
        parent = node->parent;
        grandparent = parent->parent;
      }
      parent->color = RB_BLACK;
      grandparent->color = RB_RED;
      rb_rotate_right(root, grandparent);
    } else {
      struct rb_node *uncle = grandparent->left;
      if(rb_is_red(uncle)){
        parent->color = RB_BLACK;
        uncle->color = RB_BLACK;
        grandparent->color = RB_RED;
        node = grandparent;
        continue;
      }
      if(node == parent->left){
        node = parent;
        rb_rotate_right(root, node);
        parent = node->parent;
        grandparent = parent->parent;
      }
      parent->color = RB_BLACK;
      grandparent->color = RB_RED;
      rb_rotate_left(root, grandparent);
    }
  }

  root->root->color = RB_BLACK;
}

struct rb_node *
rb_first(const struct rb_root_cached *root)
{
  return root->leftmost;
}

struct rb_node *
rb_next(const struct rb_node *node)
{
  const struct rb_node *parent;

  if(node->right){
    node = node->right;
    while(node->left)
      node = node->left;
    return (struct rb_node *)node;
  }

  parent = node->parent;
  while(parent && node == parent->right){
    node = parent;
    parent = parent->parent;
  }
  return (struct rb_node *)parent;
}

static struct rb_node *
rb_minimum(struct rb_node *node)
{
  while(node && node->left)
    node = node->left;
  return node;
}

static void
rb_transplant(struct rb_root_cached *root, struct rb_node *old,
              struct rb_node *replacement)
{
  if(old->parent == 0)
    root->root = replacement;
  else if(old == old->parent->left)
    old->parent->left = replacement;
  else
    old->parent->right = replacement;

  if(replacement)
    replacement->parent = old->parent;
}

static void
rb_erase_fixup(struct rb_root_cached *root, struct rb_node *node,
               struct rb_node *parent)
{
  while(node != root->root && rb_is_black(node)){
    struct rb_node *sibling;

    if(parent == 0)
      break;

    if(node == parent->left){
      sibling = parent->right;
      if(rb_is_red(sibling)){
        sibling->color = RB_BLACK;
        parent->color = RB_RED;
        rb_rotate_left(root, parent);
        sibling = parent->right;
      }

      if(sibling == 0){
        node = parent;
        parent = node->parent;
        continue;
      }

      if(rb_is_black(sibling->left) && rb_is_black(sibling->right)){
        sibling->color = RB_RED;
        node = parent;
        parent = node->parent;
      } else {
        if(rb_is_black(sibling->right)){
          if(sibling->left)
            sibling->left->color = RB_BLACK;
          sibling->color = RB_RED;
          rb_rotate_right(root, sibling);
          sibling = parent->right;
        }
        sibling->color = parent->color;
        parent->color = RB_BLACK;
        if(sibling->right)
          sibling->right->color = RB_BLACK;
        rb_rotate_left(root, parent);
        node = root->root;
        parent = 0;
      }
    } else {
      sibling = parent->left;
      if(rb_is_red(sibling)){
        sibling->color = RB_BLACK;
        parent->color = RB_RED;
        rb_rotate_right(root, parent);
        sibling = parent->left;
      }

      if(sibling == 0){
        node = parent;
        parent = node->parent;
        continue;
      }

      if(rb_is_black(sibling->left) && rb_is_black(sibling->right)){
        sibling->color = RB_RED;
        node = parent;
        parent = node->parent;
      } else {
        if(rb_is_black(sibling->left)){
          if(sibling->right)
            sibling->right->color = RB_BLACK;
          sibling->color = RB_RED;
          rb_rotate_left(root, sibling);
          sibling = parent->left;
        }
        sibling->color = parent->color;
        parent->color = RB_BLACK;
        if(sibling->left)
          sibling->left->color = RB_BLACK;
        rb_rotate_right(root, parent);
        node = root->root;
        parent = 0;
      }
    }
  }

  if(node)
    node->color = RB_BLACK;
}

void
rb_erase_cached(struct rb_root_cached *root, struct rb_node *node)
{
  struct rb_node *successor = node;
  struct rb_node *child;
  struct rb_node *child_parent;
  unsigned char original_color = successor->color;

  if(root->leftmost == node)
    root->leftmost = rb_next(node);

  if(node->left == 0){
    child = node->right;
    child_parent = node->parent;
    rb_transplant(root, node, node->right);
  } else if(node->right == 0){
    child = node->left;
    child_parent = node->parent;
    rb_transplant(root, node, node->left);
  } else {
    successor = rb_minimum(node->right);
    original_color = successor->color;
    child = successor->right;

    if(successor->parent == node){
      child_parent = successor;
      if(child)
        child->parent = successor;
    } else {
      child_parent = successor->parent;
      rb_transplant(root, successor, successor->right);
      successor->right = node->right;
      successor->right->parent = successor;
    }

    rb_transplant(root, node, successor);
    successor->left = node->left;
    successor->left->parent = successor;
    successor->color = node->color;
  }

  if(original_color == RB_BLACK)
    rb_erase_fixup(root, child, child_parent);

  rb_node_init(node);
  if(root->root == 0)
    root->leftmost = 0;
}
