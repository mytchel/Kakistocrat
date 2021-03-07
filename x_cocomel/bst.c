#include "../str.h"
#include "memory.h"
#include "posting.h"

#include "bst.h"

static char *str_dup_c(struct str *s) {
	uint32_t len = str_length(s);
	char *dest = (char*) memory_alloc(len + 1);
	memcpy(dest, s->store, len);
    dest[len] = '\0';
	return dest;
}

void bst_init(struct bst *b, struct str *key, uint32_t val) {
	b->left = b->right = NULL;
	b->key = str_dup_c(key);
	posting_init(&b->store);
	posting_append(&b->store, val);
}

void bst_insert(struct bst *b, struct str *key, uint32_t val, uint32_t *length)
	{
	for (;;)
		{
		int cmp = strcmp(str_c(key), b->key);

		if (cmp < 0)
			{
			if (b->left == NULL)
				{
				b->left = memory_alloc(sizeof(struct bst));
				bst_init(b->left, key, val);
				(*length)++;
				return;
				}
			b = b->left;
			}
		else if (cmp > 0)
			{
			if (b->right == NULL)
				{
				b->right = memory_alloc(sizeof(struct bst));
				bst_init(b->right, key, val);
				(*length)++;
				return;
				}
			b = b->right;
			}
		else /* |cmp == 0| */
			{
			posting_append(&b->store, val);
			return;
			}
		}
	}

char *bst_write(struct bst *b, char *start, char *ptr_buffer, char *val_buffer)
	{
	while (b != NULL)
		{
		if (b->left == NULL)
			{
			((uint32_t *)ptr_buffer)[0] = val_buffer - start;
			ptr_buffer += sizeof(uint32_t);
            strcpy(val_buffer, b->key);
			val_buffer += strlen(b->key) + 1;

			((uint32_t *)ptr_buffer)[0] = val_buffer - start;
			ptr_buffer += sizeof(uint32_t);
			val_buffer += posting_write(&b->store, val_buffer);

			b = b->right;
			}
		else
			{
			struct bst *temp = b->left;
			b->left = temp->right;
			temp->right = b;
			b = temp;
			}
		}

	return val_buffer;
	}

