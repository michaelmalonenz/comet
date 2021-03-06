#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vm.h"
#include "compiler.h"
#include "debug.h"
#include "mem.h"
#include "messages.h"
#include "import.h"

#include "comet.h"

#define PLACEHOLDER_ADDRESS 0xffff

static bool call(VM *vm, ObjClosure *closure, int argCount);
static InterpretResult run(VM *vm);

static bool create_instance(VM *vm, ObjClass *klass, int argCount)
{
    Obj *obj_instance = newInstance(vm, klass);
    if (obj_instance == NULL)
    {
        return false;
    }
    Value instance = OBJ_VAL(obj_instance);
    vm->stackTop[-argCount - 1] = instance;
    // Call the initializer, if there is one.
    Value initializer;
    if (tableGet(&klass->methods, common_strings[STRING_INIT], &initializer))
    {
        if (IS_NATIVE_METHOD(initializer))
        {
            AS_NATIVE_METHOD(initializer)->function(vm, instance, argCount, vm->stackTop - argCount);
            popMany(vm, argCount);
            return true;
        }
        else
        {
            return call(vm, AS_CLOSURE(initializer), argCount);
        }
    }
    else if (argCount != 0)
    {
        runtimeError(vm, "'%s()' expects 0 arguments but got %d.", klass->name, argCount);
        return false;
    }
    return true;
}

static void resetStack(VM *vm)
{
    vm->stackTop = vm->stack;
    vm->frameCount = 0;
    vm->openUpvalues = NULL;
}

static CallFrame *currentFrame(VM *vm)
{
    return &vm->frames[vm->frameCount - 1];
}

static bool propagateException(VM *vm)
{
    Value exception = peek(vm, 0);
    while (vm->frameCount > 0)
    {
        CallFrame *frame = currentFrame(vm);
        for (int numHandlers = frame->handlerCount; numHandlers > 0; numHandlers--)
        {
            ExceptionHandler handler = frame->handlerStack[numHandlers - 1];
            if (instanceof(exception, handler.klass) == TRUE_VAL)
            {
                frame->ip = &frame->closure->function->chunk.code[handler.handlerAddress];
                return true;
            }
            else if (handler.finallyAddress != PLACEHOLDER_ADDRESS)
            {
                push(vm, TRUE_VAL); // continue propagating once the finally block completes
                frame->ip = &frame->closure->function->chunk.code[handler.finallyAddress];
                return true;
            }
        }
        vm->frameCount--;
    }
    printf("Unhandled %s\n", AS_INSTANCE(exception)->klass->name);
    VALUE message = exception_get_message(vm, exception, 0, NULL);
    if (message != NIL_VAL)
    {
        printf("%s\n", string_get_cstr(message));
    }
    Value stacktrace = exception_get_stacktrace(vm, exception);
    if (stacktrace == NIL_VAL)
    {
        printf("No stacktrace found - did you rethrow something not previously thrown?\n");
    }
    else
    {
        printf("%s", string_get_cstr(stacktrace));
    }
    fflush(stdout);
    return false;
}

Value getStackTrace(VM *vm)
{
#define MAX_LINE_LENGTH 512
    int maxStacktraceLength = vm->frameCount * MAX_LINE_LENGTH;
    char *stacktrace = ALLOCATE(char, maxStacktraceLength);
    uint16_t index = 0;
    for (int i = vm->frameCount - 1; i >= 0; i--)
    {
        CallFrame *frame = &vm->frames[i];
        ObjFunction *function = frame->closure->function;
        // -1 because the IP is sitting on the next instruction to be
        // executed.
        size_t instruction = frame->ip - function->chunk.code - 1;
        uint32_t lineno = function->chunk.lines[instruction];
        index += snprintf(
            &stacktrace[index],
            MAX_LINE_LENGTH,
            "%s:%d - %s()\n",
            function->chunk.filename,
            lineno,
            function->name == NIL_VAL ? "script" : string_get_cstr(function->name));
    }
    stacktrace = GROW_ARRAY(stacktrace, char, maxStacktraceLength, index);
    return takeString(vm, stacktrace, index);
#undef MAX_LINE_LENGTH
}

void throw_exception_native(VM *vm, const char *exception_type_name, const char *message_format, ...)
{
    Value type_name = copyString(vm, exception_type_name, strlen(exception_type_name));
    Value exception_type = NIL_VAL;
    if (findGlobal(type_name, &exception_type))
    {
        va_list args;
        va_start(args, message_format);
        char *message = make_message(message_format, args);
        push(vm, string_create(vm, message, strlen(message)));
        create_instance(vm, AS_CLASS(exception_type), 1);
        va_end(args);
        exception_set_stacktrace(vm, peek(vm, 0), getStackTrace(vm));
        propagateException(vm);
    }
    else
    {
        runtimeError(vm, "Could not find any type named '%s'\n", exception_type_name);
    }
}

void runtimeError(VM *vm, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
    fprintf(stderr, "%s", string_get_cstr(getStackTrace(vm)));
    resetStack(vm);
}

void initVM(VM *vm)
{
    resetStack(vm);
    vm->objects = NULL;

    register_thread(vm);
}

void freeVM(VM *vm)
{
    freeObjects(vm);
    deregister_thread(vm);
}

void push(VM *vm, Value value)
{
    *vm->stackTop = value;
    vm->stackTop++;
}

Value pop(VM *vm)
{
    vm->stackTop--;
    return *vm->stackTop;
}

Value popMany(VM *vm, int count)
{
    vm->stackTop -= count;
    return *vm->stackTop;
}

Value peek(VM *vm, int distance)
{
    return vm->stackTop[-1 - distance];
}

static bool call(VM *vm, ObjClosure *closure, int argCount)
{
    if (argCount != closure->function->arity)
    {
        runtimeError(vm, "'%s' Expects %d arguments to but got %d.",
                     string_get_cstr(closure->function->name),
                     closure->function->arity,
                     argCount);
        return false;
    }

    if (vm->frameCount == FRAMES_MAX)
    {
        runtimeError(vm, "Stack overflow.");
        return false;
    }

    CallFrame *frame = &vm->frames[vm->frameCount++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;

    frame->slots = vm->stackTop - argCount - 1;
    return true;
}

static bool callValue(VM *vm, Value callee, int argCount)
{
    if (IS_OBJ(callee))
    {
        switch (OBJ_TYPE(callee))
        {
        case OBJ_BOUND_METHOD:
        {
            ObjBoundMethod *bound = AS_BOUND_METHOD(callee);

            // Replace the bound method with the receiver so it's in the
            // right slot when the method is called.
            vm->stackTop[-argCount - 1] = bound->receiver;
            return call(vm, bound->method, argCount);
        }
        case OBJ_NATIVE_CLASS:
        case OBJ_CLASS:
        {
            ObjClass *klass = AS_CLASS(callee);
            return create_instance(vm, klass, argCount);
        }
        case OBJ_CLOSURE:
            return call(vm, AS_CLOSURE(callee), argCount);

        case OBJ_NATIVE:
        {
            NativeFn native = AS_NATIVE(callee);
            Value result = native(vm, argCount, vm->stackTop - argCount);
            popMany(vm, argCount + 1);
            push(vm, result);
            return true;
        }

        default:
            printObject(callee);
            printf("\nObj type: %s\n", objTypeName(OBJ_TYPE(callee)));
            // Non-callable object type.
            break;
        }
    }

    runtimeError(vm, "Can only call functions and classes.");
    return false;
}

static bool callNativeMethod(VM *vm, Value receiver, ObjNativeMethod *method, int argCount)
{
    if (argCount < method->arity)
    {
        runtimeError(vm, "%s expects at least %u argument(s), but was given %d\n",
                     string_get_cstr(method->name), method->arity, argCount);
    }
    Value result = method->function(vm, receiver, argCount, vm->stackTop - argCount);
    popMany(vm, argCount + 1);
    push(vm, result);
    return true;
}

static Value findMethod(ObjClass *klass, Value name)
{
    Value result;
    if (tableGet(&klass->methods, name, &result))
    {
        return result;
    }
    return NIL_VAL;
}

static Value findStaticMethod(ObjClass *klass, Value name)
{
    Value result;
    if (tableGet(&klass->staticMethods, name, &result))
    {
        return result;
    }
    return NIL_VAL;
}

static bool invokeFromClass(VM *vm, ObjClass *klass, Value name,
                            int argCount)
{
    Value method = findStaticMethod(klass, name);
    if (IS_NATIVE_METHOD(method) && AS_NATIVE_METHOD(method)->isStatic)
    {
        return callNativeMethod(vm, OBJ_VAL(klass), AS_NATIVE_METHOD(method), argCount);
    }

    if (IS_BOUND_METHOD(method) || IS_CLOSURE(method))
    {
        return call(vm, AS_CLOSURE(method), argCount);
    }

    if (IS_NIL(method))
    {
        runtimeError(vm, "'%s' has no method called '%s'.", klass->name, string_get_cstr(name));
        return false;
    }

    runtimeError(vm, "Can't call method '%s' from '%s'", string_get_cstr(name), klass->name);
    return false;
}

static bool callOperator(VM *vm, Value receiver, int argCount, OPERATOR operator)
{
    if (IS_INSTANCE(receiver) || IS_NATIVE_INSTANCE(receiver))
    {
        ObjInstance *instance = AS_INSTANCE(receiver);
        Value operator_callable = instance->klass->operators[operator];
        if (IS_NIL(operator_callable))
        {
            runtimeError(vm, "Operator '%s' is not defined for class '%s'.",
                getOperatorString(operator), instance->klass->name);
            return false;
        }
        else if (IS_NATIVE_METHOD(operator_callable))
        {
            return callNativeMethod(vm, receiver, AS_NATIVE_METHOD(operator_callable), argCount);
        }
        else if (IS_CLOSURE(operator_callable))
        {
            return call(vm, AS_CLOSURE(operator_callable), argCount);
        }
        else
        {
            runtimeError(vm, "Could not call operator '%s' with '%s' (%d) on a %s instance\n",
                         getOperatorString(operator),
                         objTypeName(operator_callable),
                         AS_OBJ(operator_callable)->type,
                         instance->klass->name);
            return false;
        }
    }
    runtimeError(vm, "Operators can only be called on object instances, got '%s'", objTypeName(AS_OBJ(receiver)->type));
    return false;
}

static bool callBinaryOperator(VM *vm, OPERATOR operator)
{
    return callOperator(vm, peek(vm, 1), 1, operator);
}

static bool invoke(VM *vm, Value name, int argCount)
{
    Value receiver = peek(vm, argCount);

    if (!(IS_INSTANCE(receiver) ||
          IS_NATIVE_INSTANCE(receiver) ||
          IS_CLASS(receiver) ||
          IS_NATIVE_CLASS(receiver)))
    {
        if (IS_NIL(receiver))
        {
            runtimeError(vm, "'%s' can't be invoked from nil.", string_get_cstr(name));
        }
        else
        {
            runtimeError(vm, "'%s' can't be invoked from a '%s'.", string_get_cstr(name), objTypeName(OBJ_TYPE(receiver)));
        }
        return false;
    }

    if (IS_INSTANCE(receiver) || IS_NATIVE_INSTANCE(receiver))
    {
        ObjInstance *instance = AS_INSTANCE(receiver);

        // First look for a field which may shadow a method.
        Value value;
        if (tableGet(&instance->fields, name, &value))
        {
            // Load the field onto the stack in place of the receiver.
            vm->stackTop[-argCount - 1] = value;
            // Try to invoke it like a function.
            return callValue(vm, value, argCount);
        }

        Value method = findMethod(instance->klass, name);
        if (IS_BOUND_METHOD(method) || IS_CLOSURE(method))
        {
            return call(vm, AS_CLOSURE(method), argCount);
        }

        if (IS_NATIVE_METHOD(method))
        {
            return callNativeMethod(vm, receiver, AS_NATIVE_METHOD(method), argCount);
        }

        return invokeFromClass(vm, instance->klass, name, argCount);
    }
    return invokeFromClass(vm, AS_CLASS(receiver), name, argCount);
}

static bool bindMethod(VM *vm, ObjClass *klass, Value name)
{
    Value method;
    if (!tableGet(&klass->methods, name, &method))
    {
        runtimeError(vm, "Undefined method '%s'.", string_get_cstr(name));
        return false;
    }

    ObjBoundMethod *bound = newBoundMethod(vm, peek(vm, 0), AS_CLOSURE(method));
    pop(vm); // Instance.
    push(vm, OBJ_VAL(bound));
    return true;
}

static ObjUpvalue *captureUpvalue(VM *vm, Value *local)
{
    ObjUpvalue *prevUpvalue = NULL;
    ObjUpvalue *upvalue = vm->openUpvalues;

    while (upvalue != NULL && upvalue->location > local)
    {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local)
        return upvalue;

    ObjUpvalue *createdUpvalue = newUpvalue(vm, local);
    createdUpvalue->next = upvalue;
    if (prevUpvalue == NULL)
    {
        vm->openUpvalues = createdUpvalue;
    }
    else
    {
        prevUpvalue->next = createdUpvalue;
    }
    return createdUpvalue;
}

static void closeUpvalues(VM *vm, Value *last)
{
    while (vm->openUpvalues != NULL &&
           vm->openUpvalues->location >= last)
    {
        ObjUpvalue *upvalue = vm->openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm->openUpvalues = upvalue->next;
    }
}

void defineMethod(VM *vm, Value name, bool isStatic)
{
    Value method = peek(vm, 0);
    ObjClass *klass = AS_CLASS(peek(vm, 1));
    if (isStatic)
    {
        tableSet(&klass->staticMethods, name, method);
    }
    else
    {
        tableSet(&klass->methods, name, method);
    }
    pop(vm);
}

void defineOperator(VM *vm, OPERATOR operator)
{
    Value method = peek(vm, 0);
    ObjClass *klass = AS_CLASS(peek(vm, 1));
    klass->operators[operator] = method;
    pop(vm);
}

static void pushExceptionHandler(VM *vm, Value type, uint16_t handlerAddress, uint16_t finallyAddress)
{
    CallFrame *frame = currentFrame(vm);
    frame->handlerStack[frame->handlerCount].handlerAddress = handlerAddress;
    frame->handlerStack[frame->handlerCount].finallyAddress = finallyAddress;
    frame->handlerStack[frame->handlerCount].klass = type;
    frame->handlerCount++;
}

static CallFrame *updateFrame(VM *vm)
{
    return &vm->frames[vm->frameCount - 1];
}

static InterpretResult run(VM *vm)
{
    CallFrame *frame = updateFrame(vm);

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define BINARY_OP(operator)                          \
    do                                               \
    {                                                \
        if (!callBinaryOperator(vm, operator))       \
        {                                            \
            return INTERPRET_RUNTIME_ERROR;          \
        }                                            \
        frame = &vm->frames[vm->frameCount - 1];     \
    } while (false)

    for (;;)
    {
        uint8_t instruction;
#if DEBUG_TRACE_EXECUTION
        disassembleInstruction(&frame->closure->function->chunk,
                               (int)(frame->ip - frame->closure->function->chunk.code));
        printf("          ");
        for (Value *slot = vm->stack; slot < vm->stackTop; slot++)
        {
            printf("[ ");
            printObject(*slot);
            printf(" ]");
        }
        printf("\n");
#endif
        switch (instruction = READ_BYTE())
        {
        case OP_CONSTANT:
        {
            Value constant = READ_CONSTANT();
            push(vm, constant);
            break;
        }
        case OP_NIL:
            push(vm, NIL_VAL);
            break;
        case OP_TRUE:
            push(vm, TRUE_VAL);
            break;
        case OP_FALSE:
            push(vm, FALSE_VAL);
            break;
        case OP_POP:
            pop(vm);
            break;
        case OP_GET_GLOBAL:
        {
            Value name = READ_CONSTANT();
            Value value;
            if (!findModuleVariable(frame->closure->function->module, name, &value) &&
                !findGlobal(name, &value))
            {
                runtimeError(vm, "Undefined variable '%s'.", string_get_cstr(name));
                return INTERPRET_RUNTIME_ERROR;
            }
            push(vm, value);
            break;
        }
        case OP_DEFINE_GLOBAL:
        {
            Value name = READ_CONSTANT();
            addModuleVariable(frame->closure->function->module, name, peek(vm, 0));
            pop(vm);
            break;
        }
        case OP_GET_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            push(vm, frame->slots[slot]);
            break;
        }
        case OP_SET_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = peek(vm, 0);
            break;
        }
        case OP_SET_GLOBAL:
        {
            Value name = READ_CONSTANT();
            if (addGlobal(name, peek(vm, 0)))
            {
                runtimeError(vm, "Undefined variable '%s'.", string_get_cstr(name));
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_GET_UPVALUE:
        {
            uint8_t slot = READ_BYTE();
            push(vm, *frame->closure->upvalues[slot]->location);
            break;
        }
        case OP_SET_UPVALUE:
        {
            uint8_t slot = READ_BYTE();
            *frame->closure->upvalues[slot]->location = peek(vm, 0);
            break;
        }
        case OP_GET_PROPERTY:
        {
            if (!IS_INSTANCE(peek(vm, 0)) && !IS_NATIVE_INSTANCE(peek(vm, 0)))
            {
                runtimeError(vm, "Only instances have properties.");
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjInstance *instance = AS_INSTANCE(peek(vm, 0));
            Value name = READ_CONSTANT();

            Value value;
            if (tableGet(&instance->fields, name, &value))
            {
                pop(vm); // Instance.
                push(vm, value);
                break;
            }

            if (!bindMethod(vm, instance->klass, name))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_SET_PROPERTY:
        {
            if (!IS_INSTANCE(peek(vm, 1)) && !IS_NATIVE_INSTANCE(peek(vm, 1)))
            {
                runtimeError(vm, "Only instances have fields.");
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjInstance *instance = AS_INSTANCE(peek(vm, 1));
            tableSet(&instance->fields, OBJ_VAL(READ_CONSTANT()), peek(vm, 0));

            Value value = pop(vm);
            pop(vm);
            push(vm, value);
            break;
        }
        case OP_GET_SUPER:
        {
            Value name = READ_CONSTANT();
            ObjClass *superclass = AS_CLASS(pop(vm));
            if (!bindMethod(vm, superclass, name))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_EQUAL:
            BINARY_OP(OPERATOR_EQUALS);
            break;
        case OP_GREATER:
            BINARY_OP(OPERATOR_GREATER_THAN);
            break;
        case OP_GREATER_EQUAL:
            BINARY_OP(OPERATOR_GREATER_EQUAL);
            break;
        case OP_LESS:
            BINARY_OP(OPERATOR_LESS_THAN);
            break;
        case OP_LESS_EQUAL:
            BINARY_OP(OPERATOR_LESS_EQUAL);
            break;
        case OP_ADD:
            BINARY_OP(OPERATOR_PLUS);
            break;
        case OP_SUBTRACT:
            BINARY_OP(OPERATOR_MINUS);
            break;
        case OP_MULTIPLY:
            BINARY_OP(OPERATOR_MULTIPLICATION);
            break;
        case OP_DIVIDE:
            BINARY_OP(OPERATOR_DIVISION);
            break;
        case OP_MODULO:
            BINARY_OP(OPERATOR_MODULO);
            break;
        case OP_BITWISE_OR:
            BINARY_OP(OPERATOR_BITWISE_OR);
            break;
        case OP_BITWISE_AND:
            BINARY_OP(OPERATOR_BITWISE_AND);
            break;
        case OP_BITWISE_XOR:
            BINARY_OP(OPERATOR_BITWISE_XOR);
            break;
        case OP_NOT:
        {
            Value result = FALSE_VAL;
            if (bool_is_falsey(pop(vm)))
                result = TRUE_VAL;

            push(vm, result);
            break;
        }
        case OP_NEGATE:
            push(vm, create_number(vm, number_get_value(pop(vm)) * -1));
            break;
        case OP_JUMP:
        {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
            break;
        }
        case OP_JUMP_IF_FALSE:
        {
            uint16_t offset = READ_SHORT();
            if (bool_is_falsey(peek(vm, 0)))
                frame->ip += offset;
            break;
        }
        case OP_LOOP:
        {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
            break;
        }
        case OP_CALL:
        {
            int argCount = READ_BYTE();
            if (!callValue(vm, peek(vm, argCount), argCount))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = updateFrame(vm);
            break;
        }
        case OP_INVOKE:
        {
            Value method = READ_CONSTANT();
            int argCount = READ_BYTE();
            if (!invoke(vm, method, argCount))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = updateFrame(vm);
            break;
        }
        case OP_SUPER:
        {
            int argCount = READ_BYTE();
            Value method = READ_CONSTANT();
            ObjClass *superclass = AS_CLASS(pop(vm));
            if (!invokeFromClass(vm, superclass, method, argCount))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = updateFrame(vm);
            break;
        }
        case OP_CLOSURE:
        {
            ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
            ObjClosure *closure = newClosure(vm, function);
            push(vm, OBJ_VAL(closure));
            for (int i = 0; i < closure->upvalueCount; i++)
            {
                uint8_t isLocal = READ_BYTE();
                uint8_t index = READ_BYTE();
                if (isLocal)
                {
                    closure->upvalues[i] = captureUpvalue(vm, frame->slots + index);
                }
                else
                {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }
        case OP_CLOSE_UPVALUE:
            closeUpvalues(vm, vm->stackTop - 1);
            pop(vm);
            break;
        case OP_RETURN:
        {
            Value result = pop(vm);
            closeUpvalues(vm, frame->slots);
            vm->frameCount--;
            if (vm->frameCount == 0)
            {
                pop(vm);
                return INTERPRET_OK;
            }

            vm->stackTop = frame->slots;
            push(vm, result);

            frame = updateFrame(vm);
            break;
        }
        case OP_CLASS:
            push(vm, OBJ_VAL(newClass(vm, string_get_cstr(READ_CONSTANT()), CLS_USER_DEF)));
            break;
        case OP_INHERIT:
        {
            Value super_ = peek(vm, 1);
            if (!(IS_CLASS(super_) || IS_NATIVE_CLASS(super_)))
            {
                runtimeError(vm, "Superclass must be a class.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjClass *subclass = AS_CLASS(peek(vm, 0));
            ObjClass *superclass = AS_CLASS(super_);
            tableAddAll(&superclass->methods, &subclass->methods);
            tableAddAll(&superclass->staticMethods, &subclass->staticMethods);
            for (int i = 0; i < NUM_OPERATORS; i++)
            {
                subclass->operators[i] = superclass->operators[i];
            }
            subclass->super_ = superclass;
            pop(vm); // Subclass.
            break;
        }
        case OP_METHOD:
            defineMethod(vm, READ_CONSTANT(), false);
            break;
        case OP_STATIC_METHOD:
            defineMethod(vm, READ_CONSTANT(), true);
            break;
        case OP_INDEX:
        {
            int argCount = READ_BYTE();
            Value receiver = peek(vm, argCount);
            if (!callOperator(vm, receiver, argCount, OPERATOR_INDEX))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = updateFrame(vm);
            break;
        }
        case OP_INDEX_ASSIGN:
        {
            int argCount = READ_BYTE();
            Value receiver = peek(vm, argCount);
            if (!callOperator(vm, receiver, argCount, OPERATOR_INDEX_ASSIGN))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = updateFrame(vm);
            break;
        }
        case OP_DEFINE_OPERATOR:
        {
            defineOperator(vm, (OPERATOR)READ_BYTE());
            break;
        }
        case OP_THROW:
        {
            Value val = peek(vm, 0);
            exception_set_stacktrace(vm, val, getStackTrace(vm));
            if (propagateException(vm))
            {
                frame = updateFrame(vm);
                break;
            }
            return INTERPRET_RUNTIME_ERROR;
        }
        case OP_DUP_TOP:
        {
            push(vm, peek(vm, 0));
            break;
        }
        case OP_INSTANCEOF:
        {
            VALUE rhs = pop(vm);
            VALUE lhs = pop(vm);
            push(vm, instanceof(lhs, rhs));
            break;
        }
        case OP_PUSH_EXCEPTION_HANDLER:
        {
            VALUE type = READ_CONSTANT();
            uint16_t handlerAddress = READ_SHORT();
            uint16_t finallyAddress = READ_SHORT();
            Value value;
            if ((!findModuleVariable(frame->closure->function->module, type, &value) &&
                 !findGlobal(type, &value)) ||
                (!IS_CLASS(value) && !IS_NATIVE_CLASS(value)))
            {
                runtimeError(vm, "'%s' is not a type to catch", string_get_cstr(type));
                return INTERPRET_RUNTIME_ERROR;
            }
            pushExceptionHandler(vm, value, handlerAddress, finallyAddress);
            break;
        }
        case OP_POP_EXCEPTION_HANDLER:
            frame->handlerCount--;
            break;
        case OP_PROPAGATE_EXCEPTION:
            frame->handlerCount--;
            if (propagateException(vm))
            {
                frame = updateFrame(vm);
                break;
            }
            return INTERPRET_RUNTIME_ERROR;
        case OP_IMPORT:
        {
            // Check to see if the module has already been imported first
            ObjFunction *imported = import_from_file(vm, frame->closure->function->chunk.filename, peek(vm, 0));
            push(vm, OBJ_VAL(imported));
            addModule(imported->module, peek(vm, 1));
            ObjClosure *closure = newClosure(vm, imported);
            pop(vm);
            push(vm, OBJ_VAL(closure));
            callValue(vm, OBJ_VAL(closure), 0);
            pop(vm); // Should I be popping twice? 1 for filename, 1 for module function result (nil)
            break;
        }
        }
    }

#undef BINARY_OP
#undef READ_STRING
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_BYTE
}

VALUE call_function(VALUE receiver, VALUE method, int arg_count, VALUE *arguments)
{
    VM frame;
    initVM(&frame);
    push(&frame, receiver);
    VALUE result = NIL_VAL;
    for (int i = 0; i < arg_count; i++)
    {
        push(&frame, arguments[i]);
    }
    if (IS_BOUND_METHOD(method) || IS_CLOSURE(method) || IS_FUNCTION(method))
    {
        if (callValue(&frame, method, arg_count) && run(&frame) == INTERPRET_OK)
        {
            // There is a small window here where the result might be GC'd
            result = *(frame.stackTop + arg_count);
            push(&frame, result);
        }
    }
    else if (invoke(&frame, method, arg_count))
    {
        result = pop(&frame);
    }
    deregister_thread(&frame);
    incorporateObjects(&frame);
    return result;
}

InterpretResult interpret(VM *vm, const SourceFile *source)
{
    ObjFunction *function = compile(source, vm);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;

    push(vm, OBJ_VAL(function));
    ObjClosure *closure = newClosure(vm, function);
    pop(vm);
    push(vm, OBJ_VAL(closure));
    callValue(vm, OBJ_VAL(closure), 0);

    return run(vm);
}
