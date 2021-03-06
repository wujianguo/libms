//
//  ms_task.c
//  libms
//
//  Created by Jianguo Wu on 2018/11/21.
//  Copyright © 2018 wujianguo. All rights reserved.
//

#include "ms_task.h"
//#include "ms_http_pipe.h"
//#include "ms_mem_storage.h"
//#include "ms_file_storage.h"
#include "ms_memory_pool.h"


static void dispatch_reader(struct ms_task *task, struct ms_ireader *reader);
static int dispatch_pipe(struct ms_task *task, struct ms_ipipe *pipe);

//static int64_t max_len_from(struct ms_ireader *reader) {
//  return 32*1024*1024;
//}

static struct ms_task *cast_from(void *data) {
  return (struct ms_task *)data;
}

static void print_readers(struct ms_task *task) {
  QUEUE *q;
  struct ms_ireader *reader = NULL;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    MS_DBG("reader %p, %"INT64_FMT, reader, reader->pos);
  }
}

static void print_pipes(struct ms_task *task) {
  QUEUE *q;
  struct ms_ipipe *pipe = NULL;
  QUEUE_FOREACH(q, &task->pipes) {
    pipe = QUEUE_DATA(q, struct ms_ipipe, node);
    MS_DBG("pipe %p, %"INT64_FMT, pipe, pipe->get_current_pos(pipe));
  }
}

static void print_status(struct ms_task *task) {
  MS_DBG("======= task %p =======", task);
  print_pipes(task);
  print_readers(task);  
  MS_DBG("======= task %p =======", task);
}

static struct ms_ipipe *nearest_pipe(struct ms_task *task, int64_t pos, int direction) {
  QUEUE *q;
  struct ms_ipipe *pipe = NULL, *curr_pipe = NULL;
  QUEUE_FOREACH(q, &task->pipes) {
    pipe = QUEUE_DATA(q, struct ms_ipipe, node);
    if (direction * (pipe->get_current_pos(pipe) - pos) >= 0) {
      if (curr_pipe == NULL) {
        curr_pipe = pipe;
      } else {
        if (direction * (pipe->get_current_pos(pipe) - curr_pipe->get_current_pos(curr_pipe)) < 0) {
          curr_pipe = pipe;
        }
      }
    }
  }
  return curr_pipe;
}

static struct ms_ireader *nearest_reader(struct ms_task *task, int64_t pos, int direction) {
  QUEUE *q;
  struct ms_ireader *reader = NULL, *curr_reader = NULL;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    if (direction * (reader->pos + (int64_t)reader->sending - pos) >= 0) {
//      MS_DBG("%d, %"INT64_FMT",%zu,%"INT64_FMT, direction, reader->pos, reader->sending, pos);
      if (curr_reader == NULL) {
        curr_reader = reader;
      } else {
        if (direction * (reader->pos + (int64_t)reader->sending - (curr_reader->pos + (int64_t)curr_reader->sending)) < 0) {
          curr_reader = reader;
        }
      }
    }
  }
  return curr_reader;
}


static int64_t get_filesize(struct ms_ipipe *pipe) {
  struct ms_task *task = cast_from(pipe->user_data);
  return task->storage->get_filesize(task->storage);
}

static void on_header(struct ms_ipipe *pipe, struct http_message *hm) {
  struct ms_task *task = cast_from(pipe->user_data);
  struct mg_str *type = mg_get_http_header(hm, "Content-Type");
  MS_ASSERT(type);
  // TODO: if type == nil
  task->content_type = mg_strdup_nul(*type);
  
  struct mg_str *etag = mg_get_http_header(hm, "ETag");
  if (etag) {
    task->etag = mg_strdup_nul(*etag);
  }
  struct mg_str *date = mg_get_http_header(hm, "Date");
  if (date) {
    task->date = mg_strdup_nul(*date);
  }
  struct mg_str *last_modified = mg_get_http_header(hm, "Last-Modified");
  if (last_modified) {
    task->date = mg_strdup_nul(*last_modified);
  }
}

static void on_filesize(struct ms_ipipe *pipe, int64_t filesize) {
  struct ms_task *task = cast_from(pipe->user_data);
  task->storage->set_filesize(task->storage, filesize);
  QUEUE *q;
  struct ms_ireader *reader = NULL;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    reader->on_filesize(reader, filesize);
  }
}

static void on_content_size(struct ms_ipipe *pipe, int64_t pos, int64_t size) {
  struct ms_task *task = cast_from(pipe->user_data);
  
  if (pipe->get_req_len(pipe) == 0) {
    task->storage->set_filesize(task->storage, pos + size);
  } else {
    task->storage->set_content_size(task->storage, pos, size);
  }
  
  QUEUE *q;
  struct ms_ireader *reader = NULL;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    reader->on_content_size_from(reader, pos, size);
  }
}

static void on_recv(struct ms_ipipe *pipe, const char *buf, int64_t pos, size_t len) {
  struct ms_task *task = cast_from(pipe->user_data);
  // TODO: 淘汰缓存
  int hold_pos_len = 0;
  QUEUE *q;
  struct ms_ireader *reader = NULL;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    hold_pos_len += 1;
  }

  int64_t *hold_pos = (int64_t *)MS_MALLOC(sizeof(hold_pos) * hold_pos_len);
  memset(hold_pos, 0, sizeof(hold_pos) * hold_pos_len);
  int index = 0;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    hold_pos[index] = reader->pos + reader->sending;
  }

  task->storage->clear_buffer_for(task->storage, pos, len, hold_pos, hold_pos_len);
  size_t write = task->storage->write(task->storage, buf, pos, len);
  MS_ASSERT(write == len);
  
  
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    reader->on_recv(reader, pos, len); // TODO: on_recv maybe remove this pipe?
  }
  
  dispatch_pipe(task, pipe);
}

static void on_redirect(struct ms_ipipe *pipe, struct mg_str location) {
  struct ms_task *task = cast_from(pipe->user_data);
  if (task->redirect_url.p) {
    MS_FREE((void *)task->redirect_url.p);
  }
  task->redirect_url = mg_strdup_nul(location);
}

static void on_pipe_complete(struct ms_ipipe *pipe) {
  MS_DBG("pipe:%p", pipe);
  struct ms_task *task = cast_from(pipe->user_data);

  QUEUE_REMOVE(&pipe->node);
  pipe->close(pipe);
  
  QUEUE *q;
  struct ms_ireader *reader = NULL;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    dispatch_reader(task, reader);
  }
}

static void on_close(struct ms_ipipe *pipe, int code) {
  struct ms_task *task = cast_from(pipe->user_data);
  MS_DBG("task:%p pipe:%p code:%d", task, pipe, code);
  if (code != 0) {
    task->code = code;
  }
  QUEUE_REMOVE(&pipe->node);
  pipe->close(pipe);
  
  if (task->code == 0) {
    return;
  }
  QUEUE *q;
  struct ms_ireader *reader = NULL;
  QUEUE_FOREACH(q, &task->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    reader->on_error(reader, code);
  }
}

static void create_pipe_for(struct ms_task *task, struct ms_ireader *reader, int64_t pos, int64_t len) {
  if (task->code != 0) {
    MS_DBG("task:%p, reader:%p task->code:%d, return", task, reader, task->code);
    return;
  }
  struct ms_ipipe_callback callback = {
    get_filesize,
    on_header,
    on_filesize,
    on_content_size,
    on_recv,
    on_redirect,
    on_pipe_complete,
    on_close
  };
  
//  struct ms_ipipe *pipe = (struct ms_ipipe *)ms_http_pipe_create(task->url, pos, len, callback);
  struct mg_str *url = &task->url;
  if (task->redirect_url.len > 0) {
    url = &task->redirect_url;
  }
  struct ms_ipipe *pipe = task->factory.open_pipe(*url, pos, len, callback);
//  QUEUE_INIT(&pipe->node);
  QUEUE_INSERT_TAIL(&task->pipes, &pipe->node);
  pipe->user_data = task;
//  pipe->on_content_size = on_content_size;
//  pipe->on_recv = on_recv;
//  pipe->get_filesize = get_filesize;
//  pipe->on_complete = on_pipe_complete;
//  pipe->on_close = on_close;

  pipe->connect(pipe);
//  ms_http_pipe_connect(pipe);
}

static void dispatch_reader(struct ms_task *task, struct ms_ireader *reader) {
  // create pipe if need
  
//  MS_DBG("task:%p, reader:%p, pos:%"INT64_FMT" len:%"INT64_FMT" sending:%zu", task, reader, reader->pos, reader->len, reader->sending);
  // TODO: req_len == 0
  int64_t pos = reader->pos + reader->sending;
//  int64_t end = reader->pos + reader->sending + max_len_from(reader);
  int64_t end = reader->pos + reader->sending + task->storage->max_cache_len(task->storage);
  
  if (end > task->storage->get_estimate_size(task->storage)) {
    end = task->storage->get_estimate_size(task->storage);
  }
  int64_t cached_next_pos = 0;
  int64_t cached_next_len = 0;
  if (task->storage->get_estimate_size(task->storage) > 0) {
    task->storage->cached_from(task->storage, pos, &cached_next_pos, &cached_next_len);
    
    if (cached_next_pos == pos && cached_next_len >= task->storage->max_cache_len(task->storage)/2) {
//    if (cached_next_pos == pos && cached_next_len >= max_len_from(reader)/2) {
//      MS_DBG("task:%p, reader:%p pos:%"INT64_FMT" len:%"INT64_FMT" return", task, reader, cached_next_pos, cached_next_len);
      return;
    }
    
    if (cached_next_pos == pos && cached_next_len > 0) {
      pos += cached_next_len;
    }

  }
  if (pos > 0 && pos == end) {
//    MS_DBG("task:%p, reader:%p completed", task, reader);
    return;
  }
//  pos = reader->req_pos - reader->req_pos % MS_PIECE_UNIT_SIZE;
  pos = pos - pos % MS_PIECE_UNIT_SIZE;
  struct ms_ipipe *near_pipe = nearest_pipe(task, pos, 1);
  if (near_pipe && near_pipe->get_current_pos(near_pipe) == pos) {
//    MS_DBG("task:%p, reader:%p find near return", task, reader);
    return;
  }
  
  if (pos >= end && end != 0) {
//    MS_DBG("task:%p, reader:%p, pos:%"INT64_FMT" >= end:%"INT64_FMT" return", task, reader, pos, end);
    return;
  }
  
  int64_t len = 0;
  if (end > 0) {
    len = end - pos;
  }
  MS_DBG("task:%p, reader:%p pos:%"INT64_FMT" len:%"INT64_FMT, task, reader, pos, len);
  create_pipe_for(task, reader, pos, len);
}

static int dispatch_pipe(struct ms_task *task, struct ms_ipipe *pipe) {
  // remove pipe if need
  
  QUEUE *q;
  struct ms_ipipe *temp = NULL;
  QUEUE_FOREACH(q, &task->pipes) {
    temp = QUEUE_DATA(q, struct ms_ipipe, node);
    if (temp != pipe && temp->get_current_pos(temp) == pipe->get_current_pos(pipe)) {
      QUEUE_REMOVE(&pipe->node);
      MS_DBG("task:%p, pipe:%p", task, pipe);
      pipe->close(pipe);
//      ms_http_pipe_close(pipe);
      return 1;
    }
  }

  struct ms_ireader *reader = nearest_reader(task, pipe->get_current_pos(pipe), -1);
  
  if (!reader) {
    QUEUE_REMOVE(&pipe->node);
    MS_DBG("task:%p, pipe:%p", task, pipe);
    pipe->close(pipe);
//    ms_http_pipe_close(pipe);
    return 1;
  }
  
  if (reader->pos + (int64_t)reader->sending + task->storage->max_cache_len(task->storage) <= pipe->get_current_pos(pipe)) {
//  if (reader->pos + reader->sending + max_len_from(reader) <= pipe->get_current_pos(pipe)) {
    QUEUE_REMOVE(&pipe->node);
    MS_DBG("task:%p, pipe:%p", task, pipe);
    pipe->close(pipe);
    return 1;
//    ms_http_pipe_close(pipe);
  }

  if (pipe->get_current_pos(pipe) > 0) {
    struct ms_ipipe *near_pipe = nearest_pipe(task, pipe->get_current_pos(pipe) - 1, -1);
    if (near_pipe && reader->pos + (int64_t)reader->sending <= near_pipe->get_current_pos(near_pipe)) {
      QUEUE_REMOVE(&pipe->node);
      MS_DBG("task:%p, pipe:%p", task, pipe);
      pipe->close(pipe);
      return 1;
//      ms_http_pipe_close(pipe);
    }
  }
  return 0;
}

static void add_reader(struct ms_itask *task, struct ms_ireader *reader) {
  struct ms_task *t = (struct ms_task *)task;
  QUEUE_INSERT_TAIL(&t->readers, &reader->node);
  dispatch_reader(t, reader);
}

static size_t task_read(struct ms_itask *task, char *buf, int64_t pos, size_t len) {
  // TODO: should dispatch_pipe or not?
  struct ms_task *t = (struct ms_task *)task;
  
  QUEUE *q;
  struct ms_ireader *reader = NULL;
  QUEUE_FOREACH(q, &t->readers) {
    reader = QUEUE_DATA(q, struct ms_ireader, node);
    dispatch_reader(t, reader);
  }

//  dispatch_reader(t, reader);
  return t->storage->read(t->storage, buf, pos, len);
}

static int64_t get_task_filesize(struct ms_itask *task) {
  struct ms_task *t = (struct ms_task *)task;
  return t->storage->get_filesize(t->storage);
}

static int64_t get_task_estimate_size(struct ms_itask *task) {
  struct ms_task *t = (struct ms_task *)task;
  return t->storage->get_estimate_size(t->storage);
}

static int64_t get_completed_length(struct ms_itask *task) {
  struct ms_task *t = (struct ms_task *)task;
  return t->storage->get_completed_size(t->storage);
}

static struct mg_str get_content_type(struct ms_itask *task) {
  struct ms_task *t = (struct ms_task *)task;
  return t->content_type;
}

static void remove_reader(struct ms_itask *task, struct ms_ireader *reader) {
  MS_DBG("task:%p, reader:%p", task, reader);
  struct ms_task *t = (struct ms_task *)task;
  QUEUE_REMOVE(&reader->node);
  if (QUEUE_EMPTY(&t->readers)) {
    
//    ms_task_close(task, 5);
//    task->close(task, 5);
    t->close_ts = mg_time();
  }
//#define QUEUE_FOREACH(q, h)                                                   \
//for ((q) = QUEUE_NEXT(h); (q) != (h); (q) = QUEUE_NEXT(q))

  
  print_status(t);
  struct ms_ipipe *pipe;
  QUEUE *q;
  q = QUEUE_NEXT(&t->pipes);
  while (q != &t->pipes) {
    pipe = QUEUE_DATA(q, struct ms_ipipe, node);
    q = QUEUE_NEXT(q);
    dispatch_pipe(t, pipe);
  }
  print_status(t);

//  QUEUE *q;
//  struct ms_ipipe *pipe;
//  while (!QUEUE_EMPTY(&t->pipes)) {
//    q = QUEUE_HEAD(&t->pipes);
//    pipe = QUEUE_DATA(q, struct ms_ipipe, node);
//    dispatch_pipe(t, pipe);
//  }
}

static void task_close(struct ms_itask *task) {
  MS_DBG("task:%p", task);
  struct ms_task *t = (struct ms_task *)task;
  MS_ASSERT(QUEUE_EMPTY(&t->readers));
  
  QUEUE *q;
  struct ms_ipipe *pipe;
  while (!QUEUE_EMPTY(&t->pipes)) {
    q = QUEUE_HEAD(&t->pipes);
    pipe = QUEUE_DATA(q, struct ms_ipipe, node);
    QUEUE_REMOVE(&pipe->node);
    MS_DBG("task:%p, pipe:%p", task, pipe);
    pipe->close(pipe);
//    ms_http_pipe_close(pipe);
  }

  t->storage->close(t->storage);
  MS_FREE((void *)t->url.p);
  if (t->redirect_url.p) {
    MS_FREE((void *)t->redirect_url.p);
  }
  if (t->content_type.p) {
    MS_FREE((void *)t->content_type.p);
  }
  QUEUE_REMOVE(&t->node);
  MS_FREE(t);
}

static int get_errno(struct ms_itask *task) {
  return ((struct ms_task *)task)->code;
}

static char *get_bitmap(struct ms_itask *task) {
  struct ms_task *t = (struct ms_task *)task;
  return t->storage->get_bitmap(t->storage);
}

struct ms_task *ms_task_open(const struct mg_str url, struct ms_factory factory) {
  struct ms_task *task = MS_MALLOC(sizeof(struct ms_task));
  memset(task, 0, sizeof(struct ms_task));
  MS_DBG("task:%p", task);
  QUEUE_INIT(&task->readers);
  QUEUE_INIT(&task->pipes);
  QUEUE_INIT(&task->node);
  task->url = mg_strdup_nul(url);
  task->factory = factory;
  
  //  task->storage = (struct ms_istorage *)ms_file_storage_open();
  //  task->storage = (struct ms_istorage *)ms_mem_storage_open();
  task->storage = task->factory.open_storage();
  task->task.add_reader = add_reader;
  task->task.read = task_read;
  task->task.content_type = get_content_type;
  task->task.get_filesize = get_task_filesize;
  task->task.get_estimate_size = get_task_estimate_size;
  task->task.get_completed_size = get_completed_length;
  task->task.remove_reader = remove_reader;
  task->task.close = task_close;
  task->task.get_errno = get_errno;
  task->task.get_bitmap = get_bitmap;
  
//  task->created_at = time(NULL);
  time(&task->created_at);
  return task;
}
