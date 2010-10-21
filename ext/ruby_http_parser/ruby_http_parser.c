#include "ruby.h"
#include "ext_help.h"
#include "http_parser.c"

/*
 * Find the first occurrence of find in s, where the search is limited to the
 * first slen characters of s.
 *
 * NOTE: we use strnstr below for header matching so we don't need to
 * memcpy then memcmp from the input buffer
 */
#ifndef HAVE_STRNSTR
static char *strnstr(const char *s, const char *find, size_t slen) {
  char c, sc;
  size_t len;

  if ((c = *find++) != '\0') {
    len = strlen(find);
    do {
      do {
        if (slen-- < 1 || (sc = *s++) == '\0')
          return (NULL);
      } while (sc != c);
      if (len > slen)
        return (NULL);
    } while (strncmp(s, find, len) != 0);
    s--;
  }
  return ((char *)s);
}
#endif

#define GET_WRAPPER(N, from)  ParserWrapper *N = (ParserWrapper *)(from)->data;

#ifdef HAVE_RUBY_ENCODING_H
#include <ruby/encoding.h>
#define STR_NEW(str, len, rb_enc)                                     \
  ({                                                                  \
    VALUE _string;                                                    \
    rb_encoding *internal_encoding = rb_default_internal_encoding();  \
    if(rb_enc) {                                                      \
      _string = rb_enc_str_new(str, len, rb_enc);                     \
    } else {                                                          \
      _string = rb_str_new(str, len);                                 \
    }                                                                 \
    if(internal_encoding) {                                           \
      _string = rb_str_export_to_enc(_string, internal_encoding);     \
    }                                                                 \
    _string;                                                          \
  })

#else

#define STR_NEW(str, len, rb_enc) \
  rb_str_new(str, len)

#endif

#define HASH_CAT(h, k, ptr, len)                \
  do {                                          \
    VALUE __v = rb_hash_aref(h, k);             \
    if (__v != Qnil) {                          \
      rb_str_cat(__v, ptr, len);                \
    } else {                                    \
      rb_hash_aset(h, k, STR_NEW(ptr, len, 0)); \
    }                                           \
  } while(0)

typedef struct ParserWrapper {
  http_parser parser;

  VALUE request_url;
  VALUE request_path;
  VALUE query_string;
  VALUE fragment;

  VALUE headers;

  VALUE on_message_begin;
  VALUE on_headers_complete;
  VALUE on_body;
  VALUE on_message_complete;

  VALUE last_field_name;
#ifdef HAVE_RUBY_ENCODING_H
  rb_encoding *body_encoding;
#endif
  const char *last_field_name_at;
  size_t last_field_name_length;

  enum http_parser_type type;
} ParserWrapper;

void ParserWrapper_init(ParserWrapper *wrapper) {
  http_parser_init(&wrapper->parser, wrapper->type);
  wrapper->parser.status_code = 0;

  wrapper->headers = Qnil;

  wrapper->on_message_begin = Qnil;
  wrapper->on_headers_complete = Qnil;
  wrapper->on_body = Qnil;
  wrapper->on_message_complete = Qnil;

  wrapper->request_url = Qnil;
  wrapper->request_path = Qnil;
  wrapper->query_string = Qnil;
  wrapper->fragment = Qnil;

  wrapper->last_field_name = Qnil;
  wrapper->last_field_name_at = NULL;
  wrapper->last_field_name_length = 0;
}

void ParserWrapper_mark(void *data) {
  if(data) {
    ParserWrapper *wrapper = (ParserWrapper *) data;
    rb_gc_mark_maybe(wrapper->request_url);
    rb_gc_mark_maybe(wrapper->request_path);
    rb_gc_mark_maybe(wrapper->query_string);
    rb_gc_mark_maybe(wrapper->fragment);
    rb_gc_mark_maybe(wrapper->headers);
    rb_gc_mark_maybe(wrapper->on_message_begin);
    rb_gc_mark_maybe(wrapper->on_headers_complete);
    rb_gc_mark_maybe(wrapper->on_body);
    rb_gc_mark_maybe(wrapper->on_message_complete);
    rb_gc_mark_maybe(wrapper->last_field_name);
  }
}

void ParserWrapper_free(void *data) {
  if(data) {
    free(data);
  }
}

static VALUE cParser;
static VALUE cRequestParser;
static VALUE cResponseParser;

static VALUE eParseError;

static VALUE sCall;

/** Callbacks **/

int on_message_begin(http_parser *parser) {
  GET_WRAPPER(wrapper, parser);

  wrapper->request_url = rb_str_new2("");
  wrapper->request_path = rb_str_new2("");
  wrapper->query_string = rb_str_new2("");
  wrapper->fragment = rb_str_new2("");
  wrapper->headers = rb_hash_new();

  if (wrapper->on_message_begin != Qnil) {
    rb_funcall(wrapper->on_message_begin, sCall, 0);
  }

  return 0;
}

int on_url(http_parser *parser, const char *at, size_t length) {
  GET_WRAPPER(wrapper, parser);
  rb_str_cat(wrapper->request_url, at, length);
  return 0;
}

int on_path(http_parser *parser, const char *at, size_t length) {
  GET_WRAPPER(wrapper, parser);
  rb_str_cat(wrapper->request_path, at, length);
  return 0;
}

int on_query_string(http_parser *parser, const char *at, size_t length) {
  GET_WRAPPER(wrapper, parser);
  rb_str_cat(wrapper->query_string, at, length);
  return 0;
}

int on_fragment(http_parser *parser, const char *at, size_t length) {
  GET_WRAPPER(wrapper, parser);
  rb_str_cat(wrapper->fragment, at, length);
  return 0;
}

int on_header_field(http_parser *parser, const char *at, size_t length) {
  GET_WRAPPER(wrapper, parser);

  wrapper->last_field_name = Qnil;

  if (wrapper->last_field_name_at == NULL) {
    wrapper->last_field_name_at = at;
    wrapper->last_field_name_length = length;
  } else {
    wrapper->last_field_name_length += length;
  }

  return 0;
}

int on_header_value(http_parser *parser, const char *at, size_t length) {
  GET_WRAPPER(wrapper, parser);

  if (wrapper->last_field_name == Qnil) {
#ifdef HAVE_RUBY_ENCODING_H
    rb_encoding *rb_enc = NULL;
    char *enc = NULL;
    char *found = strnstr(wrapper->last_field_name_at, "Content-Type", wrapper->last_field_name_length);
    if(found) {
      enc = strnstr(at, "charset=", length);
      if (enc) {
        enc += sizeof("charset=")-1;
        size_t enc_len = (size_t)length-(enc-at);
        char encoding[enc_len+1];
        memcpy(encoding, enc, enc_len);
        encoding[enc_len] = 0;
        rb_enc = rb_enc_find(encoding);
        if (rb_enc) {
          wrapper->body_encoding = rb_enc;
        }
      }
    }
#endif

    wrapper->last_field_name = STR_NEW(wrapper->last_field_name_at, wrapper->last_field_name_length, 0);
    wrapper->last_field_name_at = NULL;
    wrapper->last_field_name_length = 0;
  }

  HASH_CAT(wrapper->headers, wrapper->last_field_name, at, length);

  return 0;
}

int on_headers_complete(http_parser *parser) {
  GET_WRAPPER(wrapper, parser);

  if (wrapper->on_headers_complete != Qnil) {
    rb_funcall(wrapper->on_headers_complete, sCall, 1, wrapper->headers);
  }

  return 0;
}

int on_body(http_parser *parser, const char *at, size_t length) {
  GET_WRAPPER(wrapper, parser);

  if (wrapper->on_body != Qnil) {
    VALUE body_chunk = Qnil;
#ifdef HAVE_RUBY_ENCODING_H
    if (wrapper->body_encoding) {
      body_chunk = STR_NEW(at, length, wrapper->body_encoding);
    } else {
      body_chunk = STR_NEW(at, length, 0);
    }
#else
    body_chunk = STR_NEW(at, length, 0);
#endif
    rb_funcall(wrapper->on_body, sCall, 1, body_chunk);
  }

  return 0;
}

int on_message_complete(http_parser *parser) {
  GET_WRAPPER(wrapper, parser);

  if (wrapper->on_message_complete != Qnil) {
    rb_funcall(wrapper->on_message_complete, sCall, 0);
  }

  return 0;
}

static http_parser_settings settings = {
  .on_message_begin = on_message_begin,
  .on_path = on_path,
  .on_query_string = on_query_string,
  .on_url = on_url,
  .on_fragment = on_fragment,
  .on_header_field = on_header_field,
  .on_header_value = on_header_value,
  .on_headers_complete = on_headers_complete,
  .on_body = on_body,
  .on_message_complete = on_message_complete
};

VALUE Parser_alloc_by_type(VALUE klass, enum http_parser_type type) {
  ParserWrapper *wrapper = ALLOC_N(ParserWrapper, 1);
  wrapper->type = type;
  wrapper->parser.data = wrapper;
#ifdef HAVE_RUBY_ENCODING_H
  wrapper->body_encoding = NULL;
#endif

  ParserWrapper_init(wrapper);

  return Data_Wrap_Struct(klass, ParserWrapper_mark, ParserWrapper_free, wrapper);
}

VALUE Parser_alloc(VALUE klass) {
  return Parser_alloc_by_type(klass, HTTP_BOTH);
}

VALUE RequestParser_alloc(VALUE klass) {
  return Parser_alloc_by_type(klass, HTTP_REQUEST);
}

VALUE ResponseParser_alloc(VALUE klass) {
  return Parser_alloc_by_type(klass, HTTP_RESPONSE);
}

VALUE Parser_execute(VALUE self, VALUE data) {
  ParserWrapper *wrapper = NULL;
  char *ptr = RSTRING_PTR(data);
  unsigned long len = RSTRING_LEN(data);

  DATA_GET(self, ParserWrapper, wrapper);

  size_t nparsed = http_parser_execute(&wrapper->parser, &settings, ptr, len);

  if (wrapper->parser.upgrade) {
    // upgrade request
  } else if (nparsed != len) {
    rb_raise(eParseError, "Could not parse data entirely");
  }

  return Qnil;
}

VALUE Parser_set_on_message_begin(VALUE self, VALUE callback) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  wrapper->on_message_begin = callback;
  return callback;
}

VALUE Parser_set_on_headers_complete(VALUE self, VALUE callback) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  wrapper->on_headers_complete = callback;
  return callback;
}

VALUE Parser_set_on_body(VALUE self, VALUE callback) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  wrapper->on_body = callback;
  return callback;
}

VALUE Parser_set_on_message_complete(VALUE self, VALUE callback) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  wrapper->on_message_complete = callback;
  return callback;
}

VALUE Parser_keep_alive_p(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  return http_should_keep_alive(&wrapper->parser) == 1 ? Qtrue : Qfalse;
}

VALUE Parser_upgrade_p(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  return wrapper->parser.upgrade == 1 ? Qtrue : Qfalse;
}

VALUE Parser_http_version(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  return rb_ary_new3(2, INT2FIX(wrapper->parser.http_major), INT2FIX(wrapper->parser.http_minor));
}

VALUE Parser_http_major(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  return INT2FIX(wrapper->parser.http_major);
}

VALUE Parser_http_minor(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  return INT2FIX(wrapper->parser.http_minor);
}

VALUE Parser_http_method(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  if (wrapper->parser.type == HTTP_REQUEST)
    return STR_NEW(http_method_str(wrapper->parser.method), strlen(http_method_str(wrapper->parser.method)), 0);
  else
    return Qnil;
}

VALUE Parser_status_code(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  if (wrapper->parser.status_code)
    return INT2FIX(wrapper->parser.status_code);
  else
    return Qnil;
}

#define DEFINE_GETTER(name)                  \
  VALUE Parser_##name(VALUE self) {          \
    ParserWrapper *wrapper = NULL;           \
    DATA_GET(self, ParserWrapper, wrapper);  \
    return wrapper->name;                    \
  }

DEFINE_GETTER(request_url);
DEFINE_GETTER(request_path);
DEFINE_GETTER(query_string);
DEFINE_GETTER(fragment);
DEFINE_GETTER(headers);

VALUE Parser_reset(VALUE self) {
  ParserWrapper *wrapper = NULL;
  DATA_GET(self, ParserWrapper, wrapper);

  ParserWrapper_init(wrapper);

  return Qtrue;
}

void Init_ruby_http_parser() {
  VALUE mHTTP = rb_define_module("HTTP");
  cParser = rb_define_class_under(mHTTP, "Parser", rb_cObject);
  cRequestParser = rb_define_class_under(mHTTP, "RequestParser", cParser);
  cResponseParser = rb_define_class_under(mHTTP, "ResponseParser", cParser);

  eParseError = rb_define_class_under(mHTTP, "ParseError", rb_eIOError);
  sCall = rb_intern("call");

  rb_define_alloc_func(cParser, Parser_alloc);
  rb_define_alloc_func(cRequestParser, RequestParser_alloc);
  rb_define_alloc_func(cResponseParser, ResponseParser_alloc);

  rb_define_method(cParser, "on_message_begin=", Parser_set_on_message_begin, 1);
  rb_define_method(cParser, "on_headers_complete=", Parser_set_on_headers_complete, 1);
  rb_define_method(cParser, "on_body=", Parser_set_on_body, 1);
  rb_define_method(cParser, "on_message_complete=", Parser_set_on_message_complete, 1);
  rb_define_method(cParser, "<<", Parser_execute, 1);

  rb_define_method(cParser, "keep_alive?", Parser_keep_alive_p, 0);
  rb_define_method(cParser, "upgrade?", Parser_upgrade_p, 0);

  rb_define_method(cParser, "http_version", Parser_http_version, 0);
  rb_define_method(cParser, "http_major", Parser_http_major, 0);
  rb_define_method(cParser, "http_minor", Parser_http_minor, 0);

  rb_define_method(cParser, "http_method", Parser_http_method, 0);
  rb_define_method(cParser, "status_code", Parser_status_code, 0);

  rb_define_method(cParser, "request_url", Parser_request_url, 0);
  rb_define_method(cParser, "request_path", Parser_request_path, 0);
  rb_define_method(cParser, "query_string", Parser_query_string, 0);
  rb_define_method(cParser, "fragment", Parser_fragment, 0);
  rb_define_method(cParser, "headers", Parser_headers, 0);

  rb_define_method(cParser, "reset!", Parser_reset, 0);
}
