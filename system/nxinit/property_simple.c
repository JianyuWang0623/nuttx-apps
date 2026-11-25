/****************************************************************************
 * apps/system/nxinit/property_simple.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "action.h"
#include "property.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct property_entry_s
{
  struct list_node node;
  FAR char *key;
  FAR char *value;
};

struct property_store_s
{
  struct list_node entries;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct property_entry_s *property_find(
  FAR struct property_store_s *store, FAR const char *key)
{
  FAR struct property_entry_s *entry;

  list_for_every_entry(&store->entries, entry, struct property_entry_s,
                       node)
    {
      if (!strcmp(entry->key, key))
        {
          return entry;
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int init_property_set(FAR struct init_poller_s *ctx,
                      FAR const char *key, FAR const char *value)
{
  FAR struct property_store_s *store = ctx->priv;
  FAR struct property_entry_s *entry;
  FAR char *copy;

  init_debug("Setprop key:%s value:%s", key, value);

  copy = strdup(value);
  if (copy == NULL)
    {
      return -ENOMEM;
    }

  entry = property_find(store, key);
  if (entry != NULL)
    {
      free(entry->value);
      entry->value = copy;
    }
  else
    {
      entry = calloc(1, sizeof(*entry));
      if (entry == NULL)
        {
          free(copy);
          return -ENOMEM;
        }

      entry->key = strdup(key);
      if (entry->key == NULL)
        {
          free(entry);
          free(copy);
          return -ENOMEM;
        }

      entry->value = copy;
      list_add_tail(&store->entries, &entry->node);
    }

  init_action_trigger_event(ctx->am, key, value);
  return 0;
}

int init_property_get(FAR struct init_poller_s *ctx,
                      FAR const char *key, FAR char *value, size_t len)
{
  FAR struct property_store_s *store = ctx->priv;
  FAR struct property_entry_s *entry;

  entry = property_find(store, key);
  if (entry == NULL)
    {
      return -ENOENT;
    }

  if (strlcpy(value, entry->value, len) >= len)
    {
      return -E2BIG;
    }

  return 0;
}

void init_property_handler(FAR struct init_poller_s *ctx)
{
  UNUSED(ctx);
}

void init_property_deinit(FAR struct init_poller_s *ctx)
{
  FAR struct property_store_s *store = ctx->priv;
  FAR struct property_entry_s *entry;
  FAR struct property_entry_s *tmp;

  if (store == NULL)
    {
      return;
    }

  list_for_every_entry_safe(&store->entries, entry, tmp,
                            struct property_entry_s, node)
    {
      list_delete(&entry->node);
      free(entry->key);
      free(entry->value);
      free(entry);
    }

  free(store);
  ctx->priv = NULL;
}

int init_property_init(FAR struct init_poller_s *ctx)
{
  FAR struct property_store_s *store;

  store = calloc(1, sizeof(*store));
  if (store == NULL)
    {
      return -ENOMEM;
    }

  list_initialize(&store->entries);
  ctx->priv = store;
  ctx->am->prop = ctx;
  return 0;
}
