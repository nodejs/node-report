#include <assert.h>

#include <nan.h>

NAN_METHOD(ThrowError) {
  Nan::ThrowError("an error occurred"); 
}

NAN_MODULE_INIT(Init) {
  Nan::SetMethod(target, "throwError", ThrowError);
}

NODE_MODULE(NODE_GYP_MODULE_NAME, Init)
