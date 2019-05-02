#include <node.h>
#include <nan.h>
#include <unistd.h>
#include <mutex>
#include "dht-sensor.h"

using namespace v8;

extern int initialized;
extern unsigned long long last_read[32];
extern float last_temperature[32];
extern float last_humidity[32];

std::mutex sensorMutex;

int _gpio_pin = 4;
int _sensor_type = 11;
int _max_retries = 3;
bool _test_fake_enabled = false;
float _fake_temperature = 0;
float _fake_humidity = 0;

long readSensor(int type, int pin, float &temperature, float &humidity) {
  if (_test_fake_enabled) {
    temperature = _fake_temperature;
    humidity = _fake_humidity;
    return 0;
  } else {
    return readDHT(type, pin, temperature, humidity);
  }
}

class ReadWorker : public Nan::AsyncWorker {
  public:
    ReadWorker(Nan::Callback *callback, int sensor_type, int gpio_pin)
      : Nan::AsyncWorker(callback), sensor_type(sensor_type),
        gpio_pin(gpio_pin) { }

    void Execute() {
      sensorMutex.lock();
      Init();
      Read();
      sensorMutex.unlock();
    }

    void HandleOKCallback() {
      Nan:: HandleScope scope;

      Local<Value> argv[3];
      argv[1] = Nan::New<Number>(temperature),
      argv[2] = Nan::New<Number>(humidity);

      if (!initialized) {
        argv[0] = Nan::Error("failed to initialize sensor");
      } else if (failed) {
        argv[0] = Nan::Error("failed to read sensor");
      } else {
        argv[0] = Nan::Null();
      }

      callback->Call(3, argv, async_resource);
    }

  private:
    int sensor_type;
    int gpio_pin;
    bool failed = false;
    float temperature = 0;
    float humidity = 0;

    void Init() {
      if (!initialized) {
        initialized = initialize() == 0;
      }
    }

    void Read() {
      if (sensor_type != 11 && sensor_type != 22) {
        SetErrorMessage("sensor type is invalid");
        return;
      }

      temperature = last_temperature[gpio_pin],
      humidity = last_humidity[gpio_pin];
      int retry = _max_retries;
      int result = 0;
      while (true) {
        result = readSensor(sensor_type, gpio_pin, temperature, humidity);
        if (result == 0 || --retry < 0) break;
        usleep(450000);
      }
      failed = result != 0;
    }
};

void ReadAsync(const Nan::FunctionCallbackInfo<Value>& args) {
  int sensor_type = Nan::To<int>(args[0]).FromJust();
  int gpio_pin = Nan::To<int>(args[1]).FromJust();
  Nan::Callback *callback = new Nan::Callback(args[2].As<Function>());

  Nan::AsyncQueueWorker(new ReadWorker(callback, sensor_type, gpio_pin));
}

void ReadSync(const Nan::FunctionCallbackInfo<Value>& args) {
  int sensor_type;
  int gpio_pin;

  if (args.Length() == 2) {
    gpio_pin = args[1]->Uint32Value(Nan::GetCurrentContext()).ToChecked();
    // TODO: validate gpio_pin

    sensor_type = args[0]->Uint32Value(Nan::GetCurrentContext()).ToChecked();
    if (sensor_type != 11 && sensor_type != 22) {
      Nan::ThrowTypeError("specified sensor type is invalid");
      return;
    }

    // initialization (on demand)
    if (!initialized) {
      initialized = initialize() == 0;
      if (!initialized) {
        Nan::ThrowTypeError("failed to initialize");
        return;
      }
    }
  } else {
    sensor_type = _sensor_type;
    gpio_pin = _gpio_pin;
  }

  float temperature = last_temperature[gpio_pin],
        humidity = last_humidity[gpio_pin];
  int retry = _max_retries;
  int result = 0;
  while (true) {
    result = readSensor(sensor_type, gpio_pin, temperature, humidity);
    if (result == 0 || --retry < 0) break;
    usleep(450000);
  }

  Local<Object> readout = Nan::New<Object>();
  readout->Set(Nan::New("humidity").ToLocalChecked(), Nan::New<Number>(humidity));
  readout->Set(Nan::New("temperature").ToLocalChecked(), Nan::New<Number>(temperature));
  readout->Set(Nan::New("isValid").ToLocalChecked(), Nan::New<Boolean>(result == 0));
  readout->Set(Nan::New("errors").ToLocalChecked(), Nan::New<Number>(_max_retries - retry));

  args.GetReturnValue().Set(readout);
}

void Read(const Nan::FunctionCallbackInfo<Value>& args) {
  int params = args.Length();
  switch(params) {
    case 0: // no parameters, use synchronous interface
    case 2: // sensor type and GPIO pin, use synchronous interface
      ReadSync(args);
      break;
    case 3:
      // sensorType, gpioPin and callback, use asynchronous interface
      ReadAsync(args);
      break;
    default:
      Nan::ThrowTypeError("invalid number of arguments");
  }
}

void SetMaxRetries(const Nan::FunctionCallbackInfo<Value>& args) {
    if (args.Length() != 1) {
			Nan::ThrowTypeError("Wrong number of arguments");
			return;
    }
    _max_retries = args[0]->Uint32Value(Nan::GetCurrentContext()).ToChecked();
}

void LegacyInitialization(const Nan::FunctionCallbackInfo<Value>& args) {
  if (!args[0]->IsNumber() || !args[1]->IsNumber()) {
    Nan::ThrowTypeError("Invalid arguments");
    return;
  }

  int sensor_type = args[0]->Uint32Value(Nan::GetCurrentContext()).ToChecked();
  if (sensor_type != 11 && sensor_type != 22) {
    Nan::ThrowTypeError("Specified sensor type is not supported");
    return;
  }
  
  // update parameters
  _sensor_type = sensor_type;
  _gpio_pin = args[1]->Uint32Value(Nan::GetCurrentContext()).ToChecked();

  if (args.Length() >= 3) {
    if (!args[2]->IsNumber()) {
      Nan::ThrowTypeError("Invalid maxRetries parameter");
      return;
    } else {
      _max_retries = args[2]->Uint32Value(Nan::GetCurrentContext()).ToChecked();
    }
  }
}

void Initialize(const Nan::FunctionCallbackInfo<Value>& args) {
  if (args.Length() < 1) {
    Nan::ThrowTypeError("Wrong number of arguments");
    return;
  } else if (args.Length() > 1) {
    LegacyInitialization(args);
    args.GetReturnValue().Set(Nan::New<Boolean>(initialize() == 0));
  } else if (!args[0]->IsObject()) {
    Nan::ThrowTypeError("Invalid argument: an object is expected");
    return;
  }

  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> options = args[0]->ToObject(context).ToLocalChecked();

  auto KEY_TEST = Nan::New("test").ToLocalChecked();
  auto KEY_FAKE = Nan::New("fake").ToLocalChecked();
  auto KEY_TEMP = Nan::New("temperature").ToLocalChecked();
  auto KEY_HUMIDITY = Nan::New("humidity").ToLocalChecked();
  if (Nan::Has(options, KEY_TEST).FromMaybe(true)) {
    initialized = 1;
    Local<Object> testKey = options->Get(context, KEY_TEST).ToLocalChecked()->ToObject(context).ToLocalChecked();
    if (Nan::Has(testKey, KEY_FAKE).FromMaybe(true)) {
      _test_fake_enabled = true;
      Local<Object> fakeKey = testKey->Get(context, KEY_FAKE).ToLocalChecked()->ToObject(context).ToLocalChecked();
      if (Nan::Has(fakeKey, KEY_TEMP).FromMaybe(true)) {
        Local<Value> temp = fakeKey->Get(context, KEY_TEMP).ToLocalChecked();
        _fake_temperature = temp->NumberValue(context).FromMaybe(0);
      } else {
        Nan::ThrowError("Test mode: temperature value must be defined for a fake");
      }

      if (Nan::Has(fakeKey, KEY_HUMIDITY).FromMaybe(true)) {
        Local<Value> humidity = fakeKey->Get(context, KEY_HUMIDITY).ToLocalChecked();
        _fake_humidity = humidity->NumberValue(context).FromMaybe(0);
      } else {
        Nan::ThrowError("Test mode: humidity value must be defined for a fake");
      }
    }
  }
}

void Init(Handle<Object> exports) {
	Nan::SetMethod(exports, "read", Read);
	Nan::SetMethod(exports, "initialize", Initialize);
  Nan::SetMethod(exports, "setMaxRetries", SetMaxRetries);
}

NODE_MODULE(node_dht_sensor, Init);
