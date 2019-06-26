#include <assert.h>

#include <node_api.h>

napi_value ThrowError(napi_env env, napi_callback_info info) {
  napi_throw_error(env, NULL, "an error occurred"); 
  return NULL;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value result;
  assert(napi_create_object(env, &result) == napi_ok);
  napi_value exported_function;
  assert(napi_create_function(env,
                              "throwError",
                              NAPI_AUTO_LENGTH,
                              ThrowError,
                              NULL,
                              &exported_function) == napi_ok);
  
  assert(napi_set_named_property(env,
                                 result,
                                 "throwError",
                                 exported_function) == napi_ok);
  return result;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
