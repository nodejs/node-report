#include <assert.h>

#include <node_api.h>

napi_value ThrowError(napi_env env, napi_callback_info info) {
  napi_throw_error(env, NULL, "an error occurred"); 
  return NULL;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_status status;
  napi_value result;
  napi_value exported_function;
  status = napi_create_object(env, &result);
  assert(status == napi_ok);
  status = napi_create_function(env,
                                "throwError",
                                NAPI_AUTO_LENGTH,
                                ThrowError,
                                NULL,
                                &exported_function);
  assert(status == napi_ok);
  status = napi_set_named_property(env,
                                   result,
                                   "throwError",
                                   exported_function);
  assert(status == napi_ok);
  return result;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
