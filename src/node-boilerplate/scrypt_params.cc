/*
scrypt_params.cc 

Copyright (C) 2013 Barry Steyn (http://doctrina.org/Scrypt-Authentication-For-Node.html)

This source code is provided 'as-is', without any express or implied
warranty. In no event will the author be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this source code must not be misrepresented; you must not
claim that you wrote the original source code. If you use this source code
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original source code.

3. This notice may not be removed or altered from any source distribution.

Barry Steyn barry.steyn@gmail.com

*/

#include <node.h>
#include <v8.h>
#include <string>

#include "scrypt_common.h"
#include "scrypt_params.h"

//Scrypt is a C library and there needs c linkings
extern "C" {
    #include "pickparams.h"
}

using namespace v8;

const size_t maxmem_default = 0;
const double maxmemfrac_default = 0.5;

//Asynchronous work request data
struct Baton {
    //Async callback function
    Persistent<Function> callback;

    //Custom data
    int result;
    size_t maxmem;
    double maxmemfrac;
    double maxtime;
    int N;
    uint32_t r;
	uint32_t p;
};

/*
 * Validates JavaScript params function and determines whether it is asynchronous or synchronous
 */
int ValidateArguments(const Arguments& args, std::string& message, size_t& maxmem, double& maxmemfrac, double& maxtime, int& callbackPosition) {
    if (args.Length() == 0) {
        message = "Wrong number of arguments: At least one argument is needed - the maxtime";
        return 1;
    }

	if (args.Length() > 0 && args[0]->IsFunction()) {
		message = "Wrong number of arguments: At least one argument is needed before the callback - the maxtime";
		return 1;
	}

    for (int i=0; i < args.Length(); i++) {
		if (i > 0 && args[i]->IsFunction()) { //An async signature
			callbackPosition = i;
			return 0;
		}

        switch(i) {
            case 0:
                //Check max_time is a number
                if (!args[i]->IsNumber()) {
                    message = "maxtime argument must be a number";
                    return 1;
                }

                //Check that maxtime is not less than or equal to zero (which would not make much sense)
                maxtime = Local<Number>(args[i]->ToNumber())->Value();
                if (maxtime <= 0) {
                    message = "maxtime must be greater than 0";
                    return 1;
                }
                
                break;   

            case 1:
                //Check max_memfac is a number
                if (!args[i]->IsNumber()) {
                    message = "max_memfrac argument must be a number";
                    return 1;
                }

                //Set mexmemfrac if possible, else set it to default
				maxmemfrac = Local<Number>(args[i]->ToNumber())->Value();
				if (maxmemfrac <=0)
					maxmemfrac = maxmemfrac_default;
                break; 
            
			case 2:
                //Check maxmem
                if (!args[i]->IsNumber()) {
                    message = "maxmem argument must be a number";
                    return 1;
                }

                //Set mexmem if possible, else set it to default
				int maxmemArg = Local<Number>(args[i]->ToNumber())->Value();
				if (maxmemArg < 0)
					maxmem = maxmem_default;
				else
					maxmem = (size_t)maxmemArg;

                break;
        }
    }

    return 0;
}

/*
 * Creates the actual JSON object that will be returned to the user
 */
inline void createJSONObject(Local<Object> &obj, const int &N, const uint32_t &r, const uint32_t &p) {
	obj = Object::New();
	obj->Set(String::NewSymbol("N"), Integer::New(N));
	obj->Set(String::NewSymbol("r"), Integer::New(r));
	obj->Set(String::NewSymbol("p"), Integer::New(p));
}

/*
 * The synchronous function interface
 */
Handle<Value> ParamsSync(HandleScope &scope, const size_t &maxmem, const double &maxmemfrac, const double &maxtime) {
	Local<Object> obj;
    int N=0;
    uint32_t r=0, p=0;
	int result = pickparams(maxmem, maxmemfrac, maxtime, &N, &r, &p); //Call to the scrypt function is here
	
	if (result) { //There has been an error
        ThrowException(
            Exception::TypeError(String::New(ScryptErrorDescr(result).c_str()))
        );
        return scope.Close(Undefined());		
	} else { 
		createJSONObject(obj, N, r, p);
		return scope.Close(obj);
	}
}


/*
 * Asynchronous: Work performed here
 */
void ParamsWork(uv_work_t* req) {
    Baton* baton = static_cast<Baton*>(req->data);
	baton->result = pickparams(baton->maxmem, baton->maxmemfrac, baton->maxtime, &baton->N, &baton->r, &baton->p); //Call to the scrypt function is here
}

/*
 * Asynchronous: Call back function for when work is finished
 */
void ParamsAsyncAfter(uv_work_t* req) {
    HandleScope scope;
    Baton* baton = static_cast<Baton*>(req->data);

    if (baton->result) { //There has been an error
        Local<Value> err = Exception::Error(String::New(ScryptErrorDescr(baton->result).c_str()));

        //Prepare the parameters for the callback function
        const unsigned argc = 1;
        Local<Value> argv[argc] = { err };

        // Wrap the callback function call in a TryCatch so that we can call
        // node's FatalException afterwards. This makes it possible to catch
        // the exception from JavaScript land using the
        // process.on('uncaughtException') event.
        TryCatch try_catch;
        baton->callback->Call(Context::GetCurrent()->Global(), argc, argv);
        if (try_catch.HasCaught()) {
            node::FatalException(try_catch);
        }
    } else {
        const unsigned argc = 2;
		Local<Object> obj;
		createJSONObject(obj, baton->N, baton->r, baton->p);
        
		Local<Value> argv[argc] = {
            Local<Value>::New(Null()),
            Local<Value>::New(obj)
        };

        TryCatch try_catch;
        baton->callback->Call(Context::GetCurrent()->Global(), argc, argv);
        if (try_catch.HasCaught()) {
            node::FatalException(try_catch);
        }
    }

    //Clean up
    baton->callback.Dispose();
    delete baton;
    delete req;
}

/*
 * Params: Parses arguments and determines what type
 *         (sync or async) this function is
 */
Handle<Value> Params(const Arguments& args) {
	HandleScope scope;
	std::string validateMessage;
    size_t maxmem = maxmem_default;
    double maxmemfrac = maxmemfrac_default;
    double maxtime = 0.0;
	int callbackPosition = -1;

	//Validate arguments and determine function type
	if (ValidateArguments(args, validateMessage, maxmem, maxmemfrac, maxtime, callbackPosition)) {
        ThrowException(
            Exception::TypeError(String::New(validateMessage.c_str()))
        );
        return scope.Close(Undefined());
	}

	if (callbackPosition == -1) { 
		//Synchronous
		return ParamsSync(scope, maxmem, maxmemfrac, maxtime);
	} else { 
		//Asynchronous

		//Arguments from JavaScript land
		Local<Function> callback = Local<Function>::Cast(args[callbackPosition]);

		//Asynchronous call baton that holds data passed to async function
		Baton* baton = new Baton();
		baton->maxtime = maxtime;
		baton->maxmemfrac = maxmemfrac;
		baton->maxmem = maxmem;
		baton->callback = Persistent<Function>::New(callback);

		//Asynchronous work request
		uv_work_t *req = new uv_work_t();
		req->data = baton;

		//Schedule work request
		int status = uv_queue_work(uv_default_loop(), req, ParamsWork, (uv_after_work_cb)ParamsAsyncAfter);
		assert(status == 0);

		return scope.Close(Undefined());
	}
}
