/**
 * @file rtos.c
 *   Implementation of a Real Time Operating System for the Arduino Mega board in the
 * Arduino environment 1.0.1.\n
 *   The implementation is dependent on the board (the controller) and the GNU C++ compiler
 * (thus the release of the Arduino environment) but should be easily portable to other
 * boards and Arduino releases. See documentation for details.
 *
 * Copyright (C) 2012 Peter Vranken (mailto:Peter_Vranken@Yahoo.de)
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
/* Module interface
 *   rtos_initializeTask
 *   rtos_initRTOS (internally called only)
 *   rtos_enableIRQTimerTic (callback with local default implementation)
 *   rtos_enableIRQUser00 (callback without default implementation)
 *   rtos_enableIRQUser00 (callback without default implementation)
 *   ISR(RTOS_ISR_SYSTEM_TIMER_TIC)
 *   rtos_suspendTaskTillTime
 *   ISR(RTOS_ISR_USER_00)
 *   ISR(RTOS_ISR_USER_01)
 *   rtos_setEvent
 *   rtos_waitForEvent
 *   rtos_getTaskOverrunCounter
 *   rtos_getStackReserve
 * Local functions
 *   prepareTaskStack
 *   checkForTaskActivation
 *   onTimerTic
 *   suspendTaskTillTime
 *   setEvent
 *   waitForEvent
 */


/*
 * Include files
 */

#include <arduino.h>
#include "rtos_assert.h"
#include "rtos.h"


/*
 * Defines
 */

/** The ID of the idle task. The ID of a task is identical with the index into the task
    array and the array of stack pointers. */
#define IDLE_TASK_ID    (RTOS_NO_TASKS)

/** A pattern byte, which is used as prefill byte of any task stack area. A simple and
    unexpensive stack usage check at runtime can be implemented by looking for up to where
    this pattern has been destroyed. Any value which is improbable to be a true stack
    contents byte can be used -- whatever this value might be. */
#define UNUSED_STACK_PATTERN 0x29

/** An important code pattern, which is used in every interrupt routine, which can result
    in a context switch. The CPU context except for the program counter is saved by pushing
    it onto the stack of the given context. The program counter is not explicitly saved:
    This code pattern needs to be used at the very beginning of a function so that the PC
    has been pushed onto the stack just before by the call of this function.\n
      @remark The function which uses this pattern must not be inlined, otherwise the PC
    would not be part of the saved context and the system would crash when trying to return
    to this context the next time!
      @remark This pattern needs to be changed only in strict accordance with the
    counterpart pattern, which pops the context from a stack back into the CPU. Both
    pattern needs to be the inverse of each other. */
#define PUSH_CONTEXT_ONTO_STACK                     \
    PUSH_CONTEXT_WITHOUT_R24R25_ONTO_STACK;         \
    asm volatile                                    \
    ( "push r24 \n\t"                               \
      "push r25 \n\t"                               \
    );
/* End of macro PUSH_CONTEXT_ONTO_STACK */


/** An important code pattern, which is used in every suspend command. The CPU context
    execpt for the register pair r24/r25 is saved by pushing it onto the stack of the given
    context. (Exception program counter: see macro #PUSH_CONTEXT_ONTO_STACK.)\n
      When returning to a context which has become un-due by invoking one of the suspend
    commands, the restore context should still be done with the other macro
    #POP_CONTEXT_FROM_STACK. However, before using this macro, the return code of the
    suspend command needs to be pushed onto the stack so that it is loaded into the CPU's
    register pair r24/r25 as part of macro #POP_CONTEXT_FROM_STACK.
      @remark The function which uses this pattern must not be inlined, otherwise the PC
    would not be part of the saved context and the system would crash when trying to return
    to this context the next time!
      @remark This pattern needs to be changed only in strict accordance with the
    counterpart pattern, which pops the context from a stack back into the CPU. */
#define PUSH_CONTEXT_WITHOUT_R24R25_ONTO_STACK         \
    asm volatile                                       \
    ( "push r0 \n\t"                                   \
      "in r0, __SREG__\n\t"                            \
      "push r0 \n\t"                                   \
      "push r1 \n\t"                                   \
      "push r2 \n\t"                                   \
      "push r3 \n\t"                                   \
      "push r4 \n\t"                                   \
      "push r5 \n\t"                                   \
      "push r6 \n\t"                                   \
      "push r7 \n\t"                                   \
      "push r8 \n\t"                                   \
      "push r9 \n\t"                                   \
      "push r10 \n\t"                                  \
      "push r11 \n\t"                                  \
      "push r12 \n\t"                                  \
      "push r13 \n\t"                                  \
      "push r14 \n\t"                                  \
      "push r15 \n\t"                                  \
      "push r16 \n\t"                                  \
      "push r17 \n\t"                                  \
      "push r18 \n\t"                                  \
      "push r19 \n\t"                                  \
      "push r20 \n\t"                                  \
      "push r21 \n\t"                                  \
      "push r22 \n\t"                                  \
      "push r23 \n\t"                                  \
      "push r26 \n\t"                                  \
      "push r27 \n\t"                                  \
      "push r28 \n\t"                                  \
      "push r29 \n\t"                                  \
      "push r30 \n\t"                                  \
      "push r31 \n\t"                                  \
    );
/* End of macro PUSH_CONTEXT_WITHOUT_R24R25_ONTO_STACK */


/** An important code pattern, which is used in every interrupt routine (including the
    suspend commands, which can be considered pseudo-software interrupts). The CPU context
    except for the program counter is restored by popping it from the stack of the given
    context. The program counter is not popped: This code pattern needs to be used at the
    very end of a function so that the PC will be restored by the return machine command
    (ret or reti).\n
      @remark The function which uses this pattern must not be inlined, otherwise the PC
    would not be part of the restored context and the system would crash.
      @remark This pattern needs to be changed only in strict accordance with the
    counterpart patterns, which push the context onto the stack. The pattern need to be
    the inverse of each other. */
#define POP_CONTEXT_FROM_STACK          \
    asm volatile                        \
    ( "pop r25 \n\t"                    \
      "pop r24 \n\t"                    \
      "pop r31 \n\t"                    \
      "pop r30 \n\t"                    \
      "pop r29 \n\t"                    \
      "pop r28 \n\t"                    \
      "pop r27 \n\t"                    \
      "pop r26 \n\t"                    \
      "pop r23 \n\t"                    \
      "pop r22 \n\t"                    \
      "pop r21 \n\t"                    \
      "pop r20 \n\t"                    \
      "pop r19 \n\t"                    \
      "pop r18 \n\t"                    \
      "pop r17 \n\t"                    \
      "pop r16 \n\t"                    \
      "pop r15 \n\t"                    \
      "pop r14 \n\t"                    \
      "pop r13 \n\t"                    \
      "pop r12 \n\t"                    \
      "pop r11 \n\t"                    \
      "pop r10 \n\t"                    \
      "pop r9 \n\t"                     \
      "pop r8 \n\t"                     \
      "pop r7 \n\t"                     \
      "pop r6 \n\t"                     \
      "pop r5 \n\t"                     \
      "pop r4 \n\t"                     \
      "pop r3 \n\t"                     \
      "pop r2 \n\t"                     \
      "pop r1 \n\t"                     \
      "pop r0 \n\t"                     \
      "out __SREG__, r0 \n\t"           \
      "pop r0 \n\t"                     \
    );
/* End of macro POP_CONTEXT_FROM_STACK */


/** An important code pattern, which is used in every interrupt routine (including the
    suspend commands, which can be considered pseudo-software interrupts). The code
    performs the actual task switch by saving the current stack pointer in a location owned
    by the left task and loading the stack pointer from a location owned by the new task
    (where its stack pointer value had been saved at initialization time or the last time
    it became inactive.).\n
      The code fragment then decides whether the new task had been inactivated by a timer
    interrupt or by a suspend command. In the latter case the return value of the suspend
    command is put onto the stack. From there it'll be loaded into the CPU when ending the
    interrupt routine.\n
      Side effects: The left task and the new task are read from the global variables
    _suspendedTaskId and _activeTaskId.\n
      Prerequisites: The use of the macro needs to be followed by a use of macro
    PUSH_RET_CODE_OF_SWITCH_CONTEXT. (Both macros have not been yoined to a single one only
    for sake of comprehensibility of the code using the code patterns.)\n
      The routine depends on a reset global interrupt flag.\n
      The implementation must be compatible with a naked function. In particular, it must
    not define any local data! */
// @todo For sake of readability, this could become two macros - although they will always be used as a pair, SWITCH_CONTEXT and PUSH_FUNCTION_RETURN_CODE
#define SWITCH_CONTEXT                                                                      \
{                                                                                           \
    /* Switch the stack pointer to the (saved) stack pointer of the new active task. */     \
    _tmpVarCToAsm_u16 = _taskAry[_activeTaskId].stackPointer;                               \
    asm volatile                                                                            \
    ( "in r0, __SP_L__ /* Save current stack pointer at known, fixed location */ \n\t"      \
      "sts _tmpVarAsmToC_u16, r0 \n\t"                                                      \
      "in r0, __SP_H__ \n\t"                                                                \
      "sts _tmpVarAsmToC_u16+1, r0 \n\t"                                                    \
      "lds r0, _tmpVarCToAsm_u16 \n\t"                                                      \
      "out __SP_L__, r0 /* Write l-byte of new stack pointer content */ \n\t"               \
      "lds r0, _tmpVarCToAsm_u16+1 \n\t"                                                    \
      "out __SP_H__, r0 /* Write h-byte of new stack pointer content */ \n\t"               \
    );                                                                                      \
    _taskAry[_suspendedTaskId].stackPointer = _tmpVarAsmToC_u16;                            \
                                                                                            \
} /* End of macro SWITCH_CONTEXT */



/** An important code pattern, which is used in every interrupt routine (including the
    suspend commands, which can be considered pseudo-software interrupts). Immediately
    after a context switch, the code fragment decides whether the task we had switch to had
    been inactivated by a timer interrupt or by a suspend command. (Only) in the latter
    case the return value of the suspend command is put onto the stack. From there it'll be
    loaded into the CPU when ending the interrupt routine.\n
      Side effects: The ID of the new task is read from the global variable _activeTaskId.\n
      Prerequisites: The use of the macro needs to be preceeded by a use of macro
    SWITCH_CONTEXT.\n
      The routine depends on a reset global interrupt flag.\n
      The implementation must be compatible with a naked function. In particular, it must
    not define any local data! */
#define PUSH_RET_CODE_OF_CONTEXT_SWITCH                                                     \
{                                                                                           \
    /* The first matter after a context switch is whether the new task became active the    \
       very first time after it had been suspended or if it became active again after being \
       temporarily only ready (but not suspended) because of being superseded by a higher   \
       prioritized task or because of a round-robin cycle.                                  \
         If this is the first activation after state suspended, we need to return the       \
       cause for release from suspended state as function return code to the task. When     \
       a task is suspended it always pauses inside the suspend command. */                  \
    _tmpVarCToAsm_u16 = _taskAry[_activeTaskId].postedEventVec;                             \
    if(_tmpVarCToAsm_u16 > 0)                                                               \
    {                                                                                       \
        /* Neither at state changes active -> ready, and nor at changes ready ->            \
           active, the event vector is touched. It'll be set only at state changes          \
           suspended -> ready. If we reset it now, we will surely not run into this if      \
           clause again after later changes active -> ready -> active. */                   \
        _taskAry[_activeTaskId].postedEventVec = 0;                                         \
                                                                                            \
        /* Yes, the new context was suspended before, i.e. it currently pauses inside a     \
           suspend command, waiting for its completion and expecting its return value.      \
           Place this value onto the new stack and let it be loaded by the restore          \
           context operation below. */                                                      \
        asm volatile                                                                        \
        ( "lds r0, _tmpVarCToAsm_u16 \n\t"      /* Read low byte of return code. */         \
          "push r0 \n\t"                        /* Push it at context position r24. */      \
          "lds r0, _tmpVarCToAsm_u16+1 \n\t"    /* Read high byte of return code. */        \
          "push r0 \n\t"                        /* Push it at context position r25. */      \
        );                                                                                  \
    } /* if(Do we need to place a suspend command's return code onto the new stack?) */     \
                                                                                            \
} /* End of macro PUSH_RET_CODE_OF_CONTEXT_SWITCH */


/* Two nested macros are used to convert a constant expression to a string which can be
   used e.g. as part of some inline assembler code.
     If for example PI is defined to be (355/113) you could use STR(PI) instead of
   "(355/113)" in the source code. ARG2STR is not called directly. */
#define ARG2STR(x) #x
#define STR(x) ARG2STR(x)



/*
 * Local type definitions
 */

/** The descriptor of any task. Contains static information like task priority class and
    dynamic information like received events, timer values etc.\n
      The application will fill an array of objects of this type. Some of the fields are
    initialized by the application and only read by the RTOS code. These fields are
    documented accordingly. All other fields can be prefilled by the application with dummy
    values (e.g. 0) -- the RTOS initialization will overwrite the dummy values with those
    values it needs for operation.\n
      After initailization the application must never touch any of the fields in the task
    objects -- the chance to cause a crash is very close to one. */
typedef struct
{
    /** The priority class this task belongs to. Priority class 255 has the highest
        possible priority and the lower the value the lower the priority.\n
          This settings has to be preset by the application at compile time or in function
        \a setup. After initialization it must never be touched any more, any change will
        result in a crash. */
    uint8_t prioClass;
    
    /** The task function as a function pointer. It is used once and only once: The task
        function is invoked the first time the task becomes active and must never end. A
        return statement would cause an immediate reset of the controller. */
    rtos_taskFunction_t taskFunction;
    
    /** The timer value triggering the task local absolute-timer event.\n
          This settings has to be preset by the application at compile time or in function
        \a setup. After initialization it must never be touched any more, any change will
        result in a crash.\n
          The initial value determines at which system timer tic the task becomes due the
        very first time. This may always by 0 (task becomes due immediately). In the use
        case of regular tasks it may however pay off to distribute the tasks on the time
        grid in order to avoid too many due tasks regularly at specific points in time. See
        documentation for more. */
    uintTime_t timeDueAt;
    
#if RTOS_ROUND_ROBIN_MODE_SUPPORTED == RTOS_FEATURE_ON
    /** The maximum time a task may be activated if it is operated in round-robin mode. The
        range is 1..max_value(uintTime_t).\n
          Specify a maximum time of 0 to switch round robin mode off for this task.\n
          Remark: Round robin like behavior is given only if there are several tasks in the
        same priority class and all tasks of this class have the round-robin mode
        activated. Otherwise it's just the limitation of execution time for an individual
        task.\n
          This settings has to be preset by the application at compile time or in function
        \a setup. After initialization it must never be touched any more, a change could
        result in a crash. */
    uintTime_t timeRoundRobin;
#endif

    /** The pointer to the preallocated stack area of the task. The area needs to be
        available all the RTOS runtime. Therfore dynamic allocation won't pay off. Consider
        to use the address of any statically defined array. There's no alignment
        constraint.\n
          This settings has to be preset by the application at compile time or in function
        \a setup. After initialization it must never be touched any more, a change could
        result in a crash. */
    uint8_t *pStackArea;
    
    /** The size in Byte of the memory area \a *pStackArea, which is reserved as stack for
        the task. Each task may have an individual stack size.\n
          This settings has to be preset by the application at compile time or in function
        \a setup. After initialization it must never be touched any more, a change could
        result in a crash. */
    uint16_t stackSize;
    
    /*
     * Internal fields start here. The application provided initialization values don't
     * matter.
     */
    /** @todo This part of the struct could be hidden in an anonymous type. Two arrays, one
        local to RTOS.c */

    /** The timer tic decremented counter triggering the task local delay-timer event. */
    uintTime_t cntDelay;
    
#if RTOS_ROUND_ROBIN_MODE_SUPPORTED == RTOS_FEATURE_ON
    /** The timer tic decremented counter triggering a task switch in round-robin mode. */
    uintTime_t cntRoundRobin;
#endif

    /** The events posted to this task. */
    uint16_t postedEventVec;
    
    /** The mask of events which will make this task due. */
    uint16_t eventMask;
    
    /** Do we need to wait for the first posted event or for all events? */
    bool waitForAnyEvent;

    /** The saved stack pointer of this task whenever it is not active. */
    uint16_t stackPointer;
    
    /** All recognized overruns of the timing of this task are recorded in this variable.
        The access to this variable is considered atomic by the implementation, therefore
        no other type than 8 Bit must be used.\n
          Task overruns are defined only in the (typical) use case of regular real time
        tasks. In all other applications of a task this value is useless and undefined.\n
          @remark Even for regular real time tasks, overruns can only be recognized with a
        certain probablity, which depends on the range of the cyclic system timer. Find a
        discussion in the documentation of type uintTime_t. */
    uint8_t cntOverrun;
    
    //uint8_t fillToPowerOfTwoSize[32-18];
    
} task_t;



/*
 * Local prototypes
 */
 
// @todo Consider to use macros for the two used attribute function decorations
static __attribute__((used, noinline)) bool onTimerTic(void);
volatile  __attribute__((naked, noinline)) uint16_t rtos_suspendTaskTillTime(uintTime_t deltaTimeTillRelease);
static __attribute__((used, noinline)) void suspendTaskTillTime(uintTime_t);
static __attribute__((used, noinline)) bool setEvent(uint16_t eventVec);
volatile __attribute__((naked, noinline)) void rtos_setEvent(uint16_t eventVec);
static void __attribute__((used, noinline)) waitForEvent(uint16_t eventMask, bool all, uintTime_t timeout);
volatile uint16_t __attribute__((naked, noinline)) rtos_waitForEvent(uint16_t eventMask, bool all, uintTime_t timeout);


/*
 * Data definitions
 */

/** The system time. A cyclic counter of the timer tics. The counter is interrupt driven.
    The unit of the time is defined only by the it triggering source and doesn't matter at
    all for the kernel. The time even don't need to be regular.\n
      The initial value is such that the time is 0 during the execution of the very first
    system timer interrupt service. This is important for getting a task startup behavior,
    which is transparent and predictable for the application. */
static uintTime_t _time = (uintTime_t)-1;

/** The one and only active task. This may be the only internally seen idle task which does
    nothing. */
static uint8_t _activeTaskId = IDLE_TASK_ID;

/** The task which is to be suspended because of a newly activated one. Only temporarily
    used in the instance of a task switch. */
static uint8_t _suspendedTaskId;

/** Array holding all due (but not active) tasks. Ordered according to their priority
    class. */
static uint8_t _dueTaskIdAryAry[RTOS_NO_PRIO_CLASSES][RTOS_MAX_NO_TASKS_IN_PRIO_CLASS];

/** Number of due tasks in the different priority classes. */
static uint8_t _noDueTasksAry[RTOS_NO_PRIO_CLASSES];

/** Array holding all currently suspended tasks. */
static uint8_t _suspendedTaskIdAry[RTOS_NO_TASKS];

/** Number of currently suspended tasks. */
static uint8_t _noSuspendedTasks;

/** Array of all the task objects. The array has one additional element to store the
    information about the implictly defined idle task.\n
      The array is partly initialized by the application repeatedly calling \a
    rtos_initializeTask. The rest of the initialization and the initialization of the idle
    task element is done in \a rtos_initRTOS. */
task_t _taskAry[RTOS_NO_TASKS+1];

/** Temporary data, internally used to pass information between assembly and C code. */
volatile uint16_t _tmpVarAsmToC_u16, _tmpVarCToAsm_u16;


/*
 * Function implementation
 */


/**
 * Prepare the still unused stack area of a new task in a way that the normal context
 * switching code will load the desired initial context into the CPU. Context switches are
 * implemented symmetrically: The left context (including program counter) is pushed onto
 * the stack of the left context and the new context is entered by expecting the same
 * things on its stack. This works well at runtime but requires a manual pre-filling of the
 * stack of the new context before it has ever been executed.\n
 *   While the CPU registers don't matter here too much is the program counter of
 * particular interest. By presetting the PC in the context data the start address of the
 * task is defined.
 *   @return
 * The value of the stack pointer of the new task after pushing the initial CPU context
 * onto the stack is returned. It needs to be stored in the context safe area belonging to
 * this task for usage by the first context switch to this task.
 *   @param pEmptyTaskStack
 * A pointer to a RAM area which is reserved for the stack data of the new task. The
 * easiest way to get such an area is to define a uint8_t array on module scope.
 *   @param stackSize
 * The number of bytes of the stack area.
 *   @param taskEntryPoint
 * The start address of the task code as a function pointer of the only type a task
 * function may have. See typedef for details.
 */

static uint8_t *prepareTaskStack( uint8_t * const pEmptyTaskStack
                                , uint16_t stackSize
                                , rtos_taskFunction_t taskEntryPoint
                                )
{
    uint8_t r;

    /* -1: We handle the stack pointer variable in the same way like the CPU does, with
       post-decrement. */
    uint8_t *sp = pEmptyTaskStack + stackSize - 1
          , *retCode;

    /* Push 3 Bytes of guard program counter, which is the reset address, 0x00000. If
       someone returns from a task, this will cause a reset of the controller (instead of
       an undertemined kind of crash).
         CAUTION: The distinction between 2 and 3 byte PC is the most relevant modification
       of the code whenporting to another AVR CPU. Many types use a 16 Bit PC. */
    * sp-- = 0x00;
    * sp-- = 0x00;
#ifdef __AVR_ATmega2560__
    * sp-- = 0x00;
#else
# error Modifcation of code for other AVR CPU required
#endif

    /* Push 3 Byte program counter of task start address onto the still empty stack of the
       new task. The order is LSB, MidSB, MSB from bottom to top of stack (where the
       stack's bottom is the highest memory address). */
    * sp-- = (uint32_t)taskEntryPoint & 0x000000ff;
    * sp-- = ((uint32_t)taskEntryPoint & 0x0000ff00) >> 8;
#ifdef __AVR_ATmega2560__
    * sp-- = ((uint32_t)taskEntryPoint & 0x00ff0000) >> 16;
#else
# error Modifcation of code for other AVR CPU required
#endif
    /* Now we have to push the initial value of r0, which is the __tmp_reg__ of the
       compiler. The value actually doesn't matter, we set it to 0. */
    * sp-- = 0;

    /* Now we have to push the initial value of the status register. The value basically
       doesn't matter, but why should we set any of the arithmetic flags? Also the global
       interrupt flag actually doesn't matter as the context switch will always enable
       global interrupts.
         Tip: Set the general purpose flag T controlled by a parameter of this function is a
       cheap way to pass a Boolean parameter to the task function. */
    * sp-- = 0x80;

    /* The next value is the initial value of r1. This register needs to be 0 -- the
       compiler's code inside a function depends on this and will crash if r1 has another
       value. This register is therefore also called __zero_reg__. */
    * sp-- = 0;

    /* All other registers nearly don't matter. We set them to 0. An exception is r24/r25,
       which are used by the compiler to pass a unit16_t parameter to a function. For all
       contexts of suspended tasks (including this one, which is a new one), the registers
       r25/r25 are not part of the context: The values of these registers will be loaded
       explicitly with the result of the suspend command immediately before the return to
       the task. */
    for(r=2; r<=23; ++r)
        * sp-- = 0;
    for(r=26; r<=31; ++r)
        * sp-- = 0;

    /* The stack is prepared. The value, the stack pointer has now needs to be returned to
       the caller. It has to be stored in the context save area of the new task as current
       stack pointer. */
    retCode = sp;

    /* The rest of the stack area doesn't matter. Nonetheless, we fill it with a specific
       pattern, which will permit to run a (a bit guessing) stack usage routine later on:
       We can look up to where the pattern has been destroyed. */
    while(sp >= pEmptyTaskStack)
        * sp-- = UNUSED_STACK_PATTERN;

    return retCode;

} /* End of prepareTaskStack. */





/**
 * Start the interrupt which clocks the system time. Timer 2 is used as interrupt source
 * with a period time of about 2 ms or a frequency of 490.1961 Hz respectively.\n
 *   This is the default implementation of the routine, which can be overloaded by the
 * application code if another interrupt or other interrupt settings should be used.
 */

void rtos_enableIRQTimerTic(void)

{
#ifdef __AVR_ATmega2560__
    /* Initialization of the system timer: Arduino (wiring.c, init()) has initialized
       timer2 to count up and down (phase correct PWM mode) with prescaler 64 and no TOP
       value (i.e. it counts from 0 till MAX=255). This leads to a call frequency of
       16e6Hz/64/510 = 490.1961 Hz, thus about 2 ms period time.
         Here, we found on this setting (in order to not disturb any PWM related libraries)
       and just enable the overflow interrupt. */
    /** @todo It is also possible to do specific initialization of any other available timer
        here and to enable the related interrupt. In which case you have to alter the name
        of the interrupt vector in use. Modify #RTOS_ISR_SYSTEM_TIMER_TIC to do so. */
    TIMSK2 |= _BV(TOIE2);
#else
# error Modifcation of code for other AVR CPU required
#endif

} /* End of rtos_enableIRQTimerTic */




/**
 * When an event has been posted to one or more of the currently suspended tasks, it might
 * easily be that some of these tasks are released and become due. This routine checks all
 * suspended tasks and reorders them into the due task lists if they are released.\n
 *   If there is at least one released tasks it might be that this task is of higher
 * priority than the currently active task -- in which case it will become the new active
 * task. This routine determines which task is now the active task.
 *   @return
 * The Boolean information wheather the active task now is another task is returned.\n
 *   If the function returns true, the old and the new active task's IDs are reported by
 * side effect: They are written into the global variables _suspendedTaskId and
 * _activeTaskId.
 */ 

static bool checkForTaskActivation()
{
    uint8_t idxSuspTask = 0;
    bool isNewActiveTask = false;

    while(idxSuspTask<_noSuspendedTasks)
    {
        task_t *pT = &_taskAry[_suspendedTaskIdAry[idxSuspTask]];
        uint16_t eventVec;
        
        /* Check if the task becomes due because of the events posted prior to calling this
           function. The optimally supported case is the more probable OR combination of
           events. */
        /* @todo The AND operation has been specified bad: AND must only refer to the
           postable events but not include the timer events. All postable events need to be
           set in either the mask and the vector of posted events OR any of the timer
           events in the mask are set in the vector of posted events. Consider if it still
           makes sense to have all events uniquely in a single vector: This decision was
           mainly taken because of the homegenous implementation -- which is no longer
           given with this specification change. */
        eventVec = pT->postedEventVec;
        if( (pT->waitForAnyEvent &&  eventVec != 0)
            ||  (!pT->waitForAnyEvent &&  eventVec == pT->eventMask)
          )
        {
            uint8_t u
                  , prio = pT->prioClass;

            /* This task becomes due. */
            
            /* Clear the current event mask; it becomes useless and will be reloaded with
               the next suspend operation in the next active state. */
            pT->eventMask = 0;
            
            /* Move the task from the list of suspended tasks to the list of due tasks of
               its priority class. */
            _dueTaskIdAryAry[prio][_noDueTasksAry[prio]++] = _suspendedTaskIdAry[idxSuspTask];
            -- _noSuspendedTasks;
            for(u=idxSuspTask; u<_noSuspendedTasks; ++u)
                _suspendedTaskIdAry[u] = _suspendedTaskIdAry[u+1];
            
            /* Since a task became due there might be a change of the active task. */
            isNewActiveTask = true;
        }
        else
        {
            /* The task remains suspended. */
            
            /* Check next suspended task, which is in this case found in the next array
               element. */ 
            ++ idxSuspTask;

        } /* End if(Did this suspended task become due?) */

    } /* End while(All suspended tasks) */

    /* Here, isNewActiveTask actually means "could be new active task". Find out now if
       there's really a new active task. */
    if(isNewActiveTask)
    {
        int8_t idxPrio;

        /* Look for the task we will return to. It's the first entry in the highest
           non-empty priority class. */
        for(idxPrio=RTOS_NO_PRIO_CLASSES-1; idxPrio>=0; --idxPrio)
        {
            if(_noDueTasksAry[idxPrio] > 0)
            {
                _suspendedTaskId = _activeTaskId;
                _activeTaskId    = _dueTaskIdAryAry[idxPrio][0];

                /* If we only entered the outermost if clause we made at least one task
                   due; these statements are thus surely reached. As the due becoming task
                   might however be of lower priority it can easily be that we nonetheless
                   don't have a task switch. */
                isNewActiveTask = _activeTaskId != _suspendedTaskId;

                break;
            }
        }
    } /* if(Is there a non-zero probability for a task switch?) */

    /* The calling interrupt service routine will do a context switch only if we return
       true. Otherwise it'll simply do a "reti" to the interrupted context and continue
       it. */
    return isNewActiveTask;

} /* End of checkForTaskActivation */






/**
 * This function is called from the system interrupt triggered by the main clock. The
 * timers of all due tasks are served and - in case they elapse - timer events are
 * generated. These events may then release some of the tasks. If so, they are placed in
 * the appropriate list of due tasks. Finally, the longest due task in the highest none
 * empty priority class is activated.
 *   @return
 * The Boolean information is returned whether we have or not have a task switch. In most
 * invokations we won't have and therefore it's worth to optimize the code for this case:
 * Don't do the expensive switch of the stack pointers.\n
 *   The most important result of the function, the ID of the active task after leaving the
 * function, is returned by side effect: The global variable _activeTaskId is updated.
 */

static bool onTimerTic(void)
{
    uint8_t idxSuspTask;

    /* Clock the system time. Cyclic overrun is intended. */
    ++ _time;

    /* Check for all suspended tasks if a timer event has to be posted. */
    for(idxSuspTask=0; idxSuspTask<_noSuspendedTasks; ++idxSuspTask)
    {
        task_t *pT = &_taskAry[_suspendedTaskIdAry[idxSuspTask]];

        /* Check for absolute timer event. */
        if(_time == pT->timeDueAt)
        {
            /* Setting the absolute timer event when it already is set looks like a task
               overrun indication. It isn't for two reasons. First, by means of available
               API calls the absolute timer event can't be AND combined with other events,
               so the event will immediately change the status to due (see below), so that
               setting it a second time will never occur. Secondary, an AND combination is
               basically possible by the kernel and would work fine, and if it would be
               used setting the timer event here multiple times could be an obvious
               possible consequence, but not an indication of a task overrun - as it were
               the other event which blocks the task.
                 For these reasons, the code doesn't double check for repeatedly setting the
               same event. */
            pT->postedEventVec |= (RTOS_EVT_ABSOLUTE_TIMER & pT->eventMask);
        }

        /* Check for delay timer event. The code here should optimally support the standard
           situation that the counter is constantly 0. */
        if(pT->cntDelay > 0)
        {
            if(-- pT->cntDelay == 0)
                pT->postedEventVec |= (RTOS_EVT_DELAY_TIMER & pT->eventMask);
        }
    } /* End for(All suspended tasks) */


#if RTOS_ROUND_ROBIN_MODE_SUPPORTED == RTOS_FEATURE_ON
    /* Round-robin: Applies only to the active task. It can become inactive, however not
       undue. If its time slice is elapsed it is put at the end of the due list in its
       priority class. */
    // @todo Implement round-robin. Shortcut isNewActiveTask if the active task is made undue: Now we just have to take the first one from the known list of due tasks in the given prio class - after having rotated the list contents.
# error Round robin is not yet implemented
#endif

    /* Check if the task becomes due because of the possibly occured timer events.
         The function has side effects: If there's a task which was suspended before and
       which is released because of the timer events and which is of higher priority than
       the one being active so far, the ID of the old and newly active task are written
       into global variables _suspendedTaskId and _activeTaskId. */
    return checkForTaskActivation();

} /* End of onTimerTic. */






/**
 * Each call of this function cyclically increments the system time of the kernel by one.\n
 *   Incrementing the system timer is an important system event. The routine will always
 * include an inspection of all suspended tasks, whether they could become due again.
 *   The cycle time of the system time is low (typically implemented as 0..255) and
 * determines the maximum delay time or timeout for a task which suspends itself and the
 * ratio of the task periods of the fastest and the slowest regular task. Furthermore it
 * determines the reliability of task overrun recognition. Task overrun events in the
 * magnitude of half the cycle time won't be recognized as such.\n
 *   The unit of the time is defined only by the it triggering source and doesn't matter at
 * all for the kernel. The time even don't need to be regular.\n
 *   @remark
 * The function needs to be called by an interrupt and can easily end with a context change,
 * i.e. the interrupt will return to another task as that it had interrupted.
 *   @remark
 * The connected interrupt is defined by macro #RTOS_ISR_SYSTEM_TIMER_TIC. This interrupt
 * needs to be disabled/enabled by the implementation of \a enterCriticalSection and \a
 * leaveCriticalSection.
 *   @remark
 * The cycle time of the system time can be influenced by the typedef of uintTime_t. Find a
 * discussion of pros and cons at the location of this typedef.
 *   @see bool onTimerTic(void)
 *   @see void enterCriticalSection(void)
 */

ISR(RTOS_ISR_SYSTEM_TIMER_TIC, ISR_NAKED)
{
    /* An ISR must not occur while we're updating the global data and checking for a
       possible task switch. To be more precise: The call of onTimerTic would just require
       to inhibit all those interrupts which might initiate a task switch. As long as no
       user defined interrupts are configured to set an RTOS event, this is only the single
       timer interrupt driving the system time. However, at latest when a task switch
       really is initiated we would need to lock all interrupts globally (as we modify the
       stack pointer in non-atomic operation). It doesn't matter to have locked all
       interrupts globally already here. */

    /* Save context onto the stack of the interrupted active task. */
    PUSH_CONTEXT_ONTO_STACK

	/* We must not exclude that the zero_reg is temporarily altered in the calling,
       arbitrarily interrupted code. To make the local code here running, we need
       to aniticpate this situation and clear the register. */
// @todo use __zero_reg__
    asm volatile 
    ("LabClrR0InOnTi: \n\t"
     "clr r1 \n\t"
    );

    /* Check for all suspended tasks if this change in time is an event for them. */
    if(onTimerTic())
    {
        /* Yes, another task becomes active with this timer tic. Switch the stack pointer
           to the (saved) stack pointer of that task. */
        SWITCH_CONTEXT
        PUSH_RET_CODE_OF_CONTEXT_SWITCH
    }

    /* The highly critical operation of modifying the stack pointer is done. From now on,
       all interrupts could safely operate on the new stack, the stack of the new task. This
       includes such an interrupt which would cause another task switch. However, early
       releasing the global interrupts here could lead to higher use of stack area if many
       task switches appear one after another. Therefore we will reenable the interrupts
       only with the final reti command. The disadvantage is probably minor (some clock
       tics less of responsiveness of the system). */
    /* @todo It's worth a consideration if too many task switches at a time can really
       happen: While restoring the new context is running, the only source for those task
       switches would be a new timer tic and this comes determinitically far in the
       future.*/

    /* The stack pointer points to the now active task (which will often be still the same
       as at function entry). The CPU context to continue with is popped from this stack. If
       there's no change in active task the entire routine call is just like any ordinary
       interrupt. */
    POP_CONTEXT_FROM_STACK
    
    /* The global interrupt enable flag is not saved across task switches, but always set
       on entry into the new or same context by using a reti rather than a ret.
         If we return to the same context, this will not mean that we harmfully change the
       state of a running context without the context knowing or willing it: If the context
       had reset the bit we would never have got here, as this is an ISR controlled by the
       bit. */
    asm volatile
    ( "reti \n\t"
    );

} /* End of ISR to increment the system time by one tic. */




/**
 * Suspend operation of software interrupt rtos_suspendTaskTillTime.\n
 *   The action of this SW interupt is placed into an own function in order to let the
 * compiler generate the stack frame required for all local data. (The stack frame
 * generation of the SW interupt needs to be inhibited in order to permit the
 * implementation of saving/restoring the task context).
 *   @return
 * The function determines which task is to be activated and records which task is left in
 * the global variables _activeTaskId and _suspendedTaskId.
 *   @param deltaTimeTillRelease
 * See software interrupt \a rtos_suspendTaskTillTime.
 *   @see
 * uint16_t rtos_suspendTaskTillTime(uintTime_t)
 *   @remark
 * This function and particularly passing the return code via a global variable will
 * operate only if all interrupts are disabled.
 */ 

static void suspendTaskTillTime(uintTime_t deltaTimeTillRelease)
{
    /* Avoid inlining under all circumstances. */
    asm("");
    
    int8_t idxPrio;
    uint8_t idxTask;
    
    /* Take the active task out of the list of due tasks. */
    task_t *pT = &_taskAry[_activeTaskId];
    uint8_t prio = pT->prioClass;
    uint8_t noDueNow = -- _noDueTasksAry[prio];
    for(idxTask=0; idxTask<noDueNow; ++idxTask)
        _dueTaskIdAryAry[prio][idxTask] = _dueTaskIdAryAry[prio][idxTask+1];
    
    /* This suspend command want a reactivation at a certain time. The new time is assigned
       by the += in the conditional expression.
         Task overrun detection: The new time must not more than half a cycle in the
       future. The test uses a signed comparison. The unsigned comparison >= 0x80 would be
       equivalent but probably less performant (TBC). */
    if((intTime_t)((pT->timeDueAt+=deltaTimeTillRelease) - _time) <= 0)
    {
        if(pT->cntOverrun < 0xff)
            ++ pT->cntOverrun;
        
        /* The wanted point in time is over. We do the best recocery which is possible: Let
           the task become due in the very next timer tic. */
        pT->timeDueAt = _time+1;
    }
    pT->eventMask = RTOS_EVT_ABSOLUTE_TIMER;
    pT->waitForAnyEvent = true;
    
    /* Put the task in the list of suspended tasks. */
    _suspendedTaskIdAry[_noSuspendedTasks++] = _activeTaskId;

    /* Record which task suspends itself for the assembly code in the calling function
       which actually switches the context. */
    _suspendedTaskId = _activeTaskId;
    
    /* Look for the task we will return to. It's the first entry in the highest non-empty
       priority class. The loop requires a signed index.
         It's not guaranteed that there is any due task. Idle is the fallback. */
    _activeTaskId = IDLE_TASK_ID;
    for(idxPrio=RTOS_NO_PRIO_CLASSES-1; idxPrio>=0; --idxPrio)
    {
        if(_noDueTasksAry[idxPrio] > 0)
        {
            _activeTaskId = _dueTaskIdAryAry[idxPrio][0];
            break;
        }
    }
} /* End of suspendTaskTillTime */




/**
 * Suspend the current task (i.e. the one which invokes this method) until a specified
 * point in time.\n
 *   Although specified as a increment in time, the time is meant absolute. The meant time
 * is the time of the last recent call of this function by this task plus the now specified
 * increment. This way of specifying the desired time of resume supports the intended use
 * case, which is the implementation of regular real time tasks: A task will suspend itself
 * at the end of the infinite loop which contains its functional code with a constant time
 * value. This (fixed) time value becomes the sample time of the task. This behavior is
 * opposed to a delay or sleep function: The execution time of the task is no time which
 * additionally elapses between two task resumes.\n
 *   The idle task can't be suspended. If it calls this function a crash would be the
 * immediate result.
 *   @return
 * The event mask of resuming events is returned. Since no combination with other events
 * than the elapsed system time is possible, this will always be RTOS_EVT_ABSOLUTE_TIMER.
 *   @param deltaTimeTillRelease
 * \a deltaTimeTillRelease specifies a time in the future at which the task will become due
 * again. To support the most relevant use case of this function, the implementation of
 * regular real time tasks, the time designation is relative. It refers to the last recent
 * absolute time at which this task had been resumed. This time is defined by the last
 * recent call of either this function or waitForEventTillTime. In the very first call of
 * the function it refers to the point in time the task was started.
 *   @see waitForEventTillTime
 *   @remark
 * It is absolutely essential that this routine is implemented as naked and noinline. See
 * http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html for details
 *   @remark
 * GCC doesn't create a stack frame for naked functions. For normal functions, the calling
 * parameter of the function is stored in such a stack frame. In the combination naked and
 * having local function parameters, GCC has a problem when generating code without
 * optimization: It doesn't generate a stack frame but still does save the local parameter
 * into the (not existing) stack frame as very first assembly operation of the function
 * code. There's absolutely no work around; when the earliest code, we can write inside
 * the function is executed, the stack is already corrupted in a harzardous way. A crash is
 * unavoidable.\n
 *   A (less helpful) discussion of the issue can be found at
 * http://lists.gnu.org/archive/html/avr-gcc-list/2012-08/msg00014.html.\n
 *   An imaginable work around is to pass data to the function by global objects.
 * This data would be task related so that filling the data object and calling this
 * function needed to be an atomic operation. A macro resetting the global interrupt flag,
 * filling the data object and calling the function would be required to do this.\n
 *   So far, we do not use this very, very ugly work around. Instead, we forbid to compile
 * the code with optimization off. Nonetheless, who will ever know or understand under
 * which circumstances, e.g. which combination of optimization flags, GCC will again
 * generate this trash-code. This issue remains a severe risk! Consequently, at any change
 * of a compiler setting you will need to inspect the assembly listing file and
 * double-check that it is proper with respect of using (better not using) the stack frame
 * for this function (and all other suspend functions).\n
 *   Another idea would be the implementation of this function completely in assembly code.
 * Doing so, we have the new problem of calling assembly code as C functions. Find an
 * example of how to do in
 * file:///M:/SVNMainRepository/Arduino/RTuinOS/trunk/RTuinOS/code/RTOS/rtos.c, revision 215.
 */
#ifndef __OPTIMIZE__
# error This code must not be compiled with optimization off. See source code comments for more
#endif

volatile uint16_t rtos_suspendTaskTillTime(uintTime_t deltaTimeTillRelease)
{
    /* This function is a pseudo-software interrupt. A true interrupt had reset the global
       interrupt enable flag, we inhibit any interrupts now. */
    asm volatile
    ( "cli \n\t"
    );
    
    /* The program counter as first element of the context is already on the stack (by
       calling this function). Save rest of context onto the stack of the interrupted
       active task. */ 
    PUSH_CONTEXT_WITHOUT_R24R25_ONTO_STACK

    /* Here, we could double-check _activeTaskId for the idle task ID and return without
       context switch if it is active. (The idle task has illicitly called a suspend
       command.). However, all implementation rates performance higher than failure
       tolerance, and so do we here. */
       
    /* The actual implementation of the task switch logic is placed into a sub-routine in
       order to benefit from the compiler generated stack frame for local variables (in
       this naked function we must not have declared any). The call of the function is
       immediately followed by some assembly code which processes the return value of the
       function, found in register pair r24/25. */
    suspendTaskTillTime(deltaTimeTillRelease);
    
    /* Switch the stack pointer to the (saved) stack pointer of the new active task and
       push the function result onto the new stack - from where it is loaded into r24/r25
       by the subsequent pop-context command. */
    SWITCH_CONTEXT
    PUSH_RET_CODE_OF_CONTEXT_SWITCH

    /* The stack pointer points to the now active task (which will often be still the same
       as at function entry). The CPU context to continue with is popped from this stack. If
       there's no change in active task the entire routine call is just like any ordinary
       interrupt. */
    POP_CONTEXT_FROM_STACK
    
    /* The global interrupt enable flag is not saved across task switches, but always set
       on entry into the new or same context by using a reti rather than a ret. */
    asm volatile
    ( "reti \n\t"
    );
    
    /* This statement is never reached. Just to avoid the warning. */
    return 0;
    
} /* End of rtos_suspendTaskTillTime. */




/**
 * Actual implentation of task suspension routine \a rtos_waitForEvent. The task is
 * suspended until a specified event occurs.\n
 *   The action of this SW interrupt is placed into an own function in order to let the
 * compiler generate the stack frame required for all local data. (The stack frame
 * generation of the SW interupt entry point needs to be inhibited in order to permit the
 * implementation of saving/restoring the task context).
 *   @return
 * The function determines which task is to be activated and records which task is left
 * (i.e. the task calling this routine) in the global variables _activeTaskId and
 * _suspendedTaskId.\n
 *   If there is a task switch the function reports this by a return value true. If there
 * is no task switch it returns false and the global variables _activeTaskId and
 * _suspendedTaskId are not touched.
 *   @param postedEventVec
 * See software interrupt \a rtos_setEvent.
 *   @see
 * void rtos_setEvent(uint16_t)
 *   @remark
 * This function and particularly passing the return codes via a global variable will
 * operate only if all interrupts are disabled.
 */ 

static bool setEvent(uint16_t postedEventVec)
{
    /* Avoid inlining under all circumstances. See attributes also. */
    asm("");
    
    uint8_t idxSuspTask;

    /* The timer events can't be set manually. */
    postedEventVec &= ~(RTOS_EVT_ABSOLUTE_TIMER | RTOS_EVT_DELAY_TIMER);
    
    /* Post events on all suspended tasks which are waiting for it. */
    for(idxSuspTask=0; idxSuspTask<_noSuspendedTasks; ++idxSuspTask)
    {
        task_t *pT = &_taskAry[_suspendedTaskIdAry[idxSuspTask]];

        pT->postedEventVec |= (postedEventVec & pT->eventMask);
    }

    /* Check if the task becomes due because of the posted events.
         The function has side effects: If there's a task which was suspended before and
       which is released because of the timer events and which is of higher priority than
       the one being active so far, the ID of the old and newly active task are written
       into global variables _suspendedTaskId and _activeTaskId. */
    return checkForTaskActivation();

} /* End of setEvent */





#if RTOS_USE_APPL_INTERRUPT_00 == RTOS_FEATURE_ON
/**
 * A conditionally compiled interrupt function. If #RTOS_USE_APPL_INTERRUPT_00 is set to
 * #RTOS_FEATURE_ON, this function implements an interrupt service routine, which posts
 * event #RTOS_EVT_ISR_USER_00 every time the interrupt occurs.\n
 *   The use case is to have a task (of high priority) which implements an infinite loop.
 * At the beginning of each loop cycle the task will suspend itself waiting for this event
 * (or maybe a timeout). The body of the loop will then handle the interrupt.\n
 *   In the AVR environment, an ISR can't be compiled independently of the actual interrupt
 * source it might be connected to. Therefore, to make this code compilable, the name of
 * the interrupt vector to be used has to be specified. Please set #RTOS_ISR_USER_00 to the
 * desired vector's name, like TIMER3_COMPA_vect. The supported vector names can be derived
 * from table 14-1 on page 105 in the CPU manual, doc2549.pdf (see http://www.atmel.com)\n
 *   CAUTION: There's only one ISR for each interrupt source. If you'd e.g. use
 * TIMER0_OVF_vect, you'd disable the time measurement routines of Arduino. Functions like
 * \a millis() or \a delay() would no longer work. Due to their global sphere of influence
 * interrupts must be chosen very carefully.
 *   @remark
 * The implementation of this ISR makes use of the code of the task called routine \a
 * rtos_setEvent. Both routines need to be maintained in strict accordance.
 *   @see
 * void rtos_setEvent(uint16_t)
 */

ISR(RTOS_ISR_USER_00, ISR_NAKED)
{
    /* The program counter as first element of the context is already on the stack (by
       calling this function). Save rest of context onto the stack of the interrupted
       active task. */ 
    PUSH_CONTEXT_ONTO_STACK

    /* The implementation of this ISR makes use of the code of the task called routine
       rtos_setEvent. (Both routines need to be maintained in strict accordance.) That
       function is executed with a constant parameter (r24/25) -- the event mask just
       containing the event which is posted by this interrupt. */
    asm volatile
    ( "ldi r24,lo8(" STR(RTOS_EVT_ISR_USER_00) ") \n\t"
      "ldi r25,hi8(" STR(RTOS_EVT_ISR_USER_00) ") \n\t"
      "rjmp LabelEntrySetEventForISR \n\t"
    );
} /* End of ISR(RTOS_ISR_USER_00) */

#endif /* RTOS_USE_APPL_INTERRUPT_00 == RTOS_FEATURE_ON */




#if RTOS_USE_APPL_INTERRUPT_01 == RTOS_FEATURE_ON
/**
 * A conditionally compiled interrupt function. If #RTOS_USE_APPL_INTERRUPT_01 is set to
 * #RTOS_FEATURE_ON, this function implements an interrupt service routine, which posts
 * event #RTOS_EVT_ISR_USER_01 every time the interrupt occurs.\n
 *   See documentation of ISR for application interrupt 0 for more details.
 */

ISR(RTOS_ISR_USER_01, ISR_NAKED)
{
    /* The program counter as first element of the context is already on the stack (by
       calling this function). Save rest of context onto the stack of the interrupted
       active task. */ 
    PUSH_CONTEXT_ONTO_STACK

    /* The implementation of this ISR makes use of the code of the task called routine
       rtos_setEvent. (Both routines need to be maintained in strict accordance.) That
       function is executed with a constant parameter (r24/25) -- the event mask just
       containing the event which is posted by this interrupt. */
    asm volatile
    ( "ldi r24,lo8(" STR(RTOS_EVT_ISR_USER_01) ") \n\t"
      "ldi r25,hi8(" STR(RTOS_EVT_ISR_USER_01) ") \n\t"
      "rjmp LabelEntrySetEventForISR \n\t"
    );
} /* End of ISR(RTOS_ISR_USER_01) */

#endif /* RTOS_USE_APPL_INTERRUPT_01 == RTOS_FEATURE_ON */




/**
 * A task (including the idle task) may post an event. The event is broadcasted to all
 * suspended tasks which are waiting for it. An event is not saved beyond that. If a task
 * suspends and starts waiting for an event which has been posted by another task just
 * before, it'll wait forever and never be resumed.\n
 *   The posted event may release another task, which may be of higher priority as the
 * event posting task. In this case \a setEvent will cause a task switch. The calling task
 * stays due but stops to be the active task. It does not become suspended (this is why
 * even the idle task may call this function). The activated task will resume by coming out
 * of the suspend command it had been invoked to wait for this event. The return value of
 * this suspend command will then tell about the event set here.\n
 *   If no task of higher priority is released by the posted event the calling task will be
 * continued immediately after execution of this method. In this case \a setEvent behaves
 * like any ordinary sub-routine.
 *   @param eventVec
 * A bit vector of posted events. Known events are defined in rtos.h. The timer events
 * RTOS_EVT_ABSOLUTE_TIMER and RTOS_EVT_DELAY_TIMER cannot be posted.
 *   @see
 * uint16_t rtos_waitForEvent(uint16_t, bool, uintTime_t)
 *   @remark
 * It is absolutely essential that this routine is implemented as naked and noinline. See
 * http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html for details
 *   @remark
 * In optimization level 0 GCC has a problem with code generation for naked functions. See
 * function \a rtos_suspendTaskTillTime for details.
 *   @remark
 * The implementation of this function is reused on machine code level by the
 * implementation of the application interrupt service routines \a ISR(RTOS_ISR_USER_nn).
 * This function needs to be maintained in strict accordance with the implementation of the
 * ISRs.
 */
#ifndef __OPTIMIZE__
# error This code must not be compiled with optimization off. See source code comments for more
#endif

volatile void rtos_setEvent(uint16_t eventVec)
{
    /* This function is a pseudo-software interrupt. A true interrupt had reset the global
       interrupt enable flag, we inhibit any interrupts now. */
    asm volatile
    ( "cli \n\t"
    );
    
    /* The program counter as first element of the context is already on the stack (by
       calling this function). Save rest of context onto the stack of the interrupted
       active task. setEvent does not return anything; we push register pair r24/25 and
       this will be restored on function exit. */ 
    PUSH_CONTEXT_ONTO_STACK

    /* The next assembler statement has not direct impact but permits to jump on machine
       code level into the middle of this function. The application ISRs make use of this:
       They push the interrupted context, load the parameter register pair r24/25 with the
       event they are assigned to and jump here to reuse the code for posting an event and
       probably initiate a task switch. */
    asm volatile
    ( "LabelEntrySetEventForISR: \n\t"
    );

    /* Check for all suspended tasks if the posted events will release them.
         The actual implementation of the function's logic is placed into a sub-routine in
       order to benefit from the compiler generated stack frame for local variables (in
       this naked function we must not have declared any). */
    if(setEvent(eventVec))
    {
        /* Yes, another task becomes active because of the posted events. Switch the stack
           pointer to the (saved) stack pointer of that task. */
        SWITCH_CONTEXT
        PUSH_RET_CODE_OF_CONTEXT_SWITCH
    }

    /* The stack pointer points to the now active task (which will often be still the same
       as at function entry). The CPU context to continue with is popped from this stack. If
       there's no change in active task the entire routine call is just like any ordinary
       interrupt. */
    POP_CONTEXT_FROM_STACK
    
    /* The global interrupt enable flag is not saved across task switches, but always set
       on entry into the new or same context by using a reti rather than a ret. */
    asm volatile
    ( "reti \n\t"
    );

} /* End of rtos_setEvent */




/**
 * Actual implentation of task suspension routine \a rtos_waitForEvent. The task is
 * suspended until a specified event occurs.\n
 *   The action of this SW interrupt is placed into an own function in order to let the
 * compiler generate the stack frame required for all local data. (The stack frame
 * generation of the SW interupt entry point needs to be inhibited in order to permit the
 * implementation of saving/restoring the task context).
 *   @return
 * The function determines which task is to be activated and records which task is left
 * (i.e. the task calling this routine) in the global variables _activeTaskId and
 * _suspendedTaskId.
 *   @param eventMask
 * See software interrupt \a rtos_waitForEvent.
 *   @param all
 * See software interrupt \a rtos_waitForEvent.
 *   @param timeout
 * See software interrupt \a rtos_waitForEvent.
 *   @see
 * uint16_t rtos_waitForEvent(uint16_t, bool, uintTime_t)
 *   @remark
 * This function and particularly passing the return codes via a global variable will
 * operate only if all interrupts are disabled.
 */ 

static void waitForEvent(uint16_t eventMask, bool all, uintTime_t timeout)
{
    /* Avoid inlining under all circumstances. See attributes also. */
    asm("");
    
    int8_t idxPrio;
    uint8_t idxTask;
    
    /* Take the active task out of the list of due tasks. */
    task_t *pT = &_taskAry[_activeTaskId];
    uint8_t prio = pT->prioClass;
    uint8_t noDueNow = -- _noDueTasksAry[prio];
    for(idxTask=0; idxTask<noDueNow; ++idxTask)
        _dueTaskIdAryAry[prio][idxTask] = _dueTaskIdAryAry[prio][idxTask+1];
    
    /* This suspend command wants a reactivation by a combination of events (which may
       include the timeout event).
         ++timeout: The call of the suspend function is in no way synchronized with the
       system clock. We define the delay to be a minimum and implement the resolution
       caused uncertainty as an additional delay. */
    if((uintTime_t)(timeout+1) != 0)
        ++ timeout;
    pT->cntDelay = timeout;
    pT->eventMask = eventMask;
    pT->waitForAnyEvent = !all;
    
    /* Put the task in the list of suspended tasks. */
    _suspendedTaskIdAry[_noSuspendedTasks++] = _activeTaskId;

    /* Record which task suspends itself for the assembly code in the calling function
       which actually switches the context. */
    _suspendedTaskId = _activeTaskId;
    
    /* Look for the task we will return to. It's the first entry in the highest non-empty
       priority class. The loop requires a signed index.
         It's not guaranteed that there is any due task. Idle is the fallback. */
    _activeTaskId = IDLE_TASK_ID;
    for(idxPrio=RTOS_NO_PRIO_CLASSES-1; idxPrio>=0; --idxPrio)
    {
        if(_noDueTasksAry[idxPrio] > 0)
        {
            _activeTaskId = _dueTaskIdAryAry[idxPrio][0];
            break;
        }
    }
} /* End of waitForEvent */




/**
 * Suspend the current task (i.e. the one which invokes this method) until a specified
 * combination of events occur.\n
 *   A task is suspended in the instance of calling this method. It specifies a list of
 * events. The task becomes due again, when either the first one or all of the specified
 * events have been posted by other tasks.\n
 *   The idle task can't be suspended. If it calls this function a crash would be the
 * immediate result.
 *   @return
 * The event mask of resuming events is returned. See \a rtos.h for a list of known events.
 *   @param eventMask
 * The bit vector of events to wait for. Needs to include the delay timer event
 * RTOS_EVT_DELAY_TIMER, if a timeout is required.
 *   @param all
 * If true, the task is made due only if all events are posted.\n
 *   CAUTION: Due to a specification error, this flag can be reasonable set in only in
 * combination of \a not specifying a timeout. Otherwise the activation condition would be
 * to wait for all events and to wait for the timeout being elapsed -- which is surely not
 * what you mean with a timeout. Waiting for any event with timeout is obviously not a
 * problem.
 *   @param timeout
 * The number of system timer tics from now on until the timeout elapses. One should be
 * aware the resolution of any timing is the tic of the system timer. A timeout of n may
 * actually mean any delay in the range n..n+1 tics.\n
 *   Even specifying 0 will suspend the task a short time and give others the chance to
 * become active.\n
 *   If RTOS_EVT_DELAY_TIMER is not set in the event mask, this parameter doesn't matter.
 *   @remark
 * It is absolutely essential that this routine is implemented as naked and noinline. See
 * http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html for details
 *   @remark
 * In optimization level 0 GCC has a problem with code generation for naked functions. See
 * function \a rtos_suspendTaskTillTime for details.
 *   @see uint16_t rtos_suspendTaskTillTime(uintTime_t)
 */
#ifndef __OPTIMIZE__
# error This code must not be compiled with optimization off. See source code comments for more
#endif

volatile uint16_t rtos_waitForEvent(uint16_t eventMask, bool all, uintTime_t timeout)
{
    /* This function is a pseudo-software interrupt. A true interrupt had reset the global
       interrupt enable flag, we inhibit any interrupts now. */
    asm volatile
    ( "cli \n\t"
    );
    
    /* The program counter as first element of the context is already on the stack (by
       calling this function). Save rest of context onto the stack of the interrupted
       active task. */ 
    PUSH_CONTEXT_WITHOUT_R24R25_ONTO_STACK

    /* Here, we could double-check _activeTaskId for the idle task ID and return without
       context switch if it is active. (The idle task has illicitly called a suspend
       command.) However, all implementation rates performance higher than failure
       tolerance, and so do we here. */
       
    /* The actual implementation of the task switch logic is placed into a sub-routine in
       order to benefit from the compiler generated stack frame for local variables (in
       this naked function we must not have declared any). The call of the function is
       immediately followed by some assembly code which processes the return value of the
       function, found in register pair r24/25. */
    waitForEvent(eventMask, all, timeout);
    
    /* Switch the stack pointer to the (saved) stack pointer of the new active task and
       push the function result onto the new stack - from where it is loaded into r24/r25
       by the subsequent pop-context command. */
    SWITCH_CONTEXT
    PUSH_RET_CODE_OF_CONTEXT_SWITCH

    /* The stack pointer points to the now active task (which will often be still the same
       as at function entry). The CPU context to continue with is popped from this stack. If
       there's no change in active task the entire routine call is just like any ordinary
       interrupt. */
    POP_CONTEXT_FROM_STACK
    
    /* The global interrupt enable flag is not saved across task switches, but always set
       on entry into the new or same context by using a reti rather than a ret. */
    asm volatile
    ( "reti \n\t"
    );
    
    /* This statement is never reached. Just to avoid the warning. */
    return 0;

} /* End of rtos_waitForEvent */







/**
 * Get the current value of the overrun counter of a given task.\n
 *   The value is a limited (i.e. it won't cycle around) 8 Bit counter. This is considered
 * satisfying as any task overrun is a kind of error and should not happen in a real
 * application (with other words: even a Boolean information would maybe enough).
 * Furthermore, if a larger range is required, one can regularly ask for this information,
 * accumulate it and reset the value here to zero at the same time.\n
 *   The function may be called from a task or from the idle task.
 *   @return
 * Get the current value of the overrun counter.
 *   @param idxTask
 * The index of the task the overrun counter if which is to be returned. The index is the
 * same as used when initializing the tasks (see rtos_initializeTask).
 *   @param doReset
 * Boolean flag, which tells whether to reset the value.\n
 *   Caution, when setting this to true, reading and resetting the value needs to become an
 * atomic operation, which requires a critical section. This is significantly more
 * expensive than just reading the value.
 *   @see
 * void rtos_initializeTask()
 */

uint8_t rtos_getTaskOverrunCounter(uint8_t idxTask, bool doReset)
{
    if(doReset)
    {
        uint8_t retCode;
        
        /* Read and reset should be atomic for data consistency if the application wants to
           accumulate the counter values in order to extend the counter's range. */
        cli();
        {
            retCode = _taskAry[idxTask].cntOverrun;    
            _taskAry[idxTask].cntOverrun = 0;
        }
        sei();
        
        return retCode;
    }
    else
    {
        /* Reading an 8 Bit word is an atomic operation as such, no additional lock
           operation needed. */
        return _taskAry[idxTask].cntOverrun;
    }
} /* End of rtos_getTaskOverrunCounter */




/**
 * Compute how many bytes of the stack area of a task are still unsued. If the value is
 * requested after an application has been run a long while and has been forced to run
 * through all its paths many times, it may be used to optimize the static stack allocation
 * of the task. The function is useful only for diagnosis purpose as there's no chance to
 * dynamically increase or decrease the stack area at runtime.\n
 *   The function may be called from a task or from the idle task.\n
 *   The alogorithm is as follows: The unused part of the stack is initialized with a
 * specific pattern byte. This routine counts the number of subsequent pattern bytes down
 * from the top of the stack area. This number is returned.\n
 *   The returned result must not be trusted too much: It could of course be that a pattern
 * byte is found not because of teh initialization but because it has been pushed onto the
 * stack - in which case the return value is too great (too optimistic) by one. The
 * probability that this happens is significanly greater than zero. The chance that two
 * pattern bytes had been pushed is however much less and the probability of three, four,
 * five such bytes in sequence is neglectable. (Except the irrelevant case you initialize
 * an automatic array variable with all pattern bytes.) Any stack size optimization based
 * on this routine should therefore subtract e.g. five bytes from the returned reserve and
 * diminish the stack outermost by this modified value.\n
 *   Be careful with stack size optimization based on this routine: Even if the application
 * ran a long time there's a non-zero probability that there has not yet been a system
 * timer interrupt in the very instance that the code of the task of interest was busy in
 * the deepest nested sub-routine, i.e. when having the largest stack consumption. A good
 * suggestion is to have another 36 Byte of reserve - this is the stack consumption if an
 * interrupt occurs.\n
 *   Recipe: Run your application a long time, ensure that it ran through all paths, get
 * the stack reserve from this routine, subtract 5+36 Byte and diminish the stack by this
 * value.
 *   @return
 * The number of still unsused stack bytes. See function description for details.
 *   @param idxTask
 * The index of the task the stack usage has to be investigated for. The index is the
 * same as used when initializing the tasks (see rtos_initializeTask).
 *   @remark
 * The computation is a linear search for the first non-pattern byte and thus relatively
 * expensive. It's suggested to call it only in some specific diagnosis compilation or
 * occasionally from the idle task.
 *   @see
 * void rtos_initializeTask()
 */

uint16_t rtos_getStackReserve(uint8_t idxTask)
{
    uint8_t *sp = _taskAry[idxTask].pStackArea;
    
    /* The bottom of the stack is always initialized with 0, which must not be the pattern
       byte. Therefore we don't need a limitation of the search loop -- it'll always find a
       non-pattern byte in the stack area. */
    while(*sp == UNUSED_STACK_PATTERN)
        ++ sp;

    return sp - _taskAry[idxTask].pStackArea;
    
} /* End of rtos_getStackReserve */




/**
 * Initialize the contents of a single task object.\n
 *   This routine needs to be called from within setup() once for each task. The number of
 * tasks has been defined by the application using #RTOS_NO_TASKS but the array of this
 * number of task objects is still empty. The system will crash if this routine is not
 * called properly for each of the tasks before the RTOS actually starts.\n
 *   This function must never be called outside of setup(). A crash would result otherwise.
 *   @param idxTask
 * The index of the task in the range 0..RTOS_NO_TASKS-1. The order of tasks barely
 * matters.
 *   @param taskFunction
 * The task function as a function pointer. It is used once and only once: The task
 * function is invoked the first time the task becomes active and must never end. A
 * return statement would cause an immediate reset of the controller.
 *   @param prioClass
 * The priority class this task belongs to. Priority class 255 has the highest
 * possible priority and the lower the value the lower the priority.
 *   @param timeRoundRobin
 * The maximum time a task may be activated if it is operated in round-robin mode. The
 * range is 1..max_value(\a uintTime_t).\n
 *   Specify a maximum time of 0 to switch round robin mode off for this task.\n
 *   Remark: Round robin like behavior is given only if there are several tasks in the
 * same priority class and all tasks of this class have the round-robin mode
 * activated. Otherwise it's just the limitation of execution time for an individual
 * task.\n
 *   This parameter is available only if #RTOS_ROUND_ROBIN_MODE_SUPPORTED is set to
 * #RTOS_FEATURE_ON.
 *   @param pStackArea
 * The pointer to the preallocated stack area of the task. The area needs to be
 * available all the RTOS runtime. Therefore dynamic allocation won't pay off. Consider
 * to use the address of any statically defined array. There's no alignment
 * constraint.
 *   @param stackSize
 * The size in Byte of the memory area \a *pStackArea, which is reserved as stack for
 * the task. Each task may have an individual stack size.
 *   @param startEventMask
 * The condition under which the task becomes due the very first time is specified in the
 * same way as at runtime when using the suspend command rtos_waitForEvent: A set of events
 * to wait for is specified, the Boolean information if any event will activate the task or
 * if all are required and finally a timeout in case no such events would be posted.\n
 *   This parameter specifies the set of events as a bit vector.
 *   @param startByAllEvents
 * If true, all specified events must be posted before the task is activated. Otherwise the
 * first event belonging to the specified set will activate the task.
 *   @param startTimeout
 * The task will be started at latest after \a startTimeout system timer tics if only the
 * event #RTOS_EVT_DELAY_TIMER is in the set of specified events. If it is not, the task
 * will not be activated by a time condition.
 *   @see void rtos_initRTOS(void)
 *   @see uint16_t rtos_waitForEvent(uint16_t, bool, uintTime_t)
 *   @remark
 */

void rtos_initializeTask( uint8_t idxTask
                        , rtos_taskFunction_t taskFunction
                        , uint8_t prioClass
#if RTOS_ROUND_ROBIN_MODE_SUPPORTED == RTOS_FEATURE_ON
                        , uintTime_t timeRoundRobin
#endif
                        , uint8_t * const pStackArea
                        , uint16_t stackSize
                        , uint16_t startEventMask
                        , bool startByAllEvents
                        , uintTime_t startTimeout
                        )
{
    task_t * const pT = &_taskAry[idxTask];
    
    /* Remember task function and stack allocation. */
    pT->taskFunction = taskFunction;
    pT->pStackArea   = pStackArea;
    pT->stackSize    = stackSize; 

    /* To which priority class does the task belong? */
    pT->prioClass = prioClass;
    
    /* Set the start condition. 
         ++ startTimer: Immediate start with the first timer tic requires an initial
       counter value of 1. (0 means not to count at all.) */
    ASSERT(startEventMask != 0);
    ASSERT((startEventMask & RTOS_EVT_ABSOLUTE_TIMER) == 0);
    pT->eventMask       = startEventMask;
    pT->waitForAnyEvent = !startByAllEvents;
    if((uintTime_t)(startTimeout+1) != 0)
        ++ startTimeout;
    pT->cntDelay = startTimeout;
    
#if RTOS_ROUND_ROBIN_MODE_SUPPORTED == RTOS_FEATURE_ON
    /* The maximum execution time in round robin mode. */
    pT->timeRoundRobin = timeRoundRobin;
#endif    

} /* End of rtos_initializeTask */




/**
 * Application called initialization of RTOS.\n
 *   Most important is the application handled task information. A task is characterized by
 * several static settings which need to be preset by the application. To save ressources,
 * a standard situation will be the specification of all relevant settings at compile time
 * in the initializer expression of the array definition. The array is declared extern to
 * enable this mode.\n
 *   The application however also has the chance to provide this information at runtime.
 * Early in the execution of this function a callback \a setup into the application is
 * invoked. If the application has setup everything as a compile time expression, the
 * callback may simply contain a return.\n
 *   The callback \a setup is invoked before any RTOS related interrupts have been
 * initialized, the routine is executed in the same context as and as a substitute of the
 * normal Arduino setup routine. The implementation can be made without bothering with
 * interrupt inhibition or data synchronization considerations. Furthermore it can be used
 * for all the sort of things you've ever done in Arduino's setup routine.\n
 *   After returning from \a setup all tasks defined in the task array are made due. The
 * main interrupt, which clocks the RTOS system time is started and will immediately make
 * the very task the active task which belongs to the highest priority class and which was
 * found first (i.e. in the order of rising indexes) in the task array. The system is
 * running.\n
 *   No idle task is specified in the task array. The idle task is implicitly defined and
 * implemented as the external function \a loop. To stick to Arduino's convention (and to
 * give the RTOS the chance to benefit from idle as well) \a loop is still implemented as
 * such: You are encouraged to return from \a loop after doing things. RTOS will call \a
 * loop again as soon as it has some time left.\n
 *   As for the original Arduino code, \a setup and \a loop are mandatory, global
 * functions.\n
 *   This routine is not called by the application but in C's main function. Your code
 * seems to start with setup and seems then to branch into either \a loop (the idle task)
 * or any other of the tasks defined by your application.\n
 *   This function never returns. No task must ever return, a reset will be the immediate
 * consequence. Your part of the idle task, function \a loop, may and should return, but
 * the actual idle task as a whole won't terminate neither. Instead it'll repeat to call \a
 * loop.
 */

void rtos_initRTOS(void)

{
    uint8_t idxTask, idxClass;
    task_t *pT;

#ifdef DEBUG
    /* We add some code to double-check that rtos_initializeTask has been invoked for each
       of the tasks. */
    memset(/* dest */ _taskAry, /* val */ 0x00, /* len */ sizeof(_taskAry));
#endif

    /* Give the application the chance to do all its initialization -- regardless of RTOS
       related or whatever else. After return, the task array needs to be properly
       filled. */
    setup();

    /* Handle all tasks. */
    for(idxTask=0; idxTask<RTOS_NO_TASKS; ++idxTask)
    {
        pT = &_taskAry[idxTask];

        ASSERT(pT->taskFunction != NULL  &&  pT->pStackArea != NULL  &&  pT->stackSize >= 50);

        /* Prepare the stack of the task and store the initial stack pointer value. */
        pT->stackPointer = (uint16_t)prepareTaskStack( pT->pStackArea
                                                     , pT->stackSize
                                                     , pT->taskFunction
                                                     );
#ifdef DEBUG
# if false
        {
            uint16_t i;

            Serial.print("Task ");
            Serial.print(idxTask);
            Serial.print(":\nStack pointer: 0x");
            Serial.println(pT->stackPointer, HEX);
            for(i=0; i<pT->stackSize; ++i)
            {
                if(i%8 == 0)
                {
                    Serial.println("");
                    Serial.print(i, HEX);
                    Serial.print(", 0x");
                    Serial.print((uint16_t)(pT->pStackArea+i), HEX);
                    Serial.print(":\t");
                }
                Serial.print(pT->pStackArea[i], HEX);
                Serial.print("\t");
            }
            Serial.println("");
        }
# endif
#endif

        /* The absolute timer is not enabled at the beginning, the value here actually
           doesn't matter. */
        pT->timeDueAt = 0;
        
#if RTOS_ROUND_ROBIN_MODE_SUPPORTED == RTOS_FEATURE_ON
        /* The round robin counter is loaded to its maximum when the tasks becomes due.
           Now, the value doesn't matter. */
        pT->cntRoundRobin = 0;
#endif
        /* No events have been posted to this task yet. */
        pT->postedEventVec = 0;

        /* Initialize overrun counter. */
        pT->cntOverrun = 0;
        
        /* Any task is suspended at the beginning. No task is active, see before. */
        _suspendedTaskIdAry[idxTask] = idxTask;

    } /* for(All tasks to initialize) */

    /* Number of currently suspended tasks: All. */
    _noSuspendedTasks = RTOS_NO_TASKS;

    /* The idle task is stored in the last array entry. It differs, there's e.g. no task
       function defined. We mainly need to storage location for the stack pointer. */
    pT = &_taskAry[IDLE_TASK_ID];
    pT->stackPointer = 0;           /* Used only at de-activation. */
    pT->timeDueAt = 0;              /* Not used at all. */
    pT->cntDelay = 0;               /* Not used at all. */
#if RTOS_ROUND_ROBIN_MODE_SUPPORTED == RTOS_FEATURE_ON
    pT->cntRoundRobin = 0;          /* Not used at all. */
#endif

    /* The next element always needs to be 0. Otherwise any interrupt or a call of setEvent
       would corrupt the stack assuming that a suspend command would require a return
       code. */
    pT->postedEventVec = 0;
    
    pT->eventMask = 0;              /* Not used at all. */
    pT->waitForAnyEvent = false;    /* Not used at all. */
    pT->cntOverrun = 0;             /* Not used at all. */

    /* Any task is suspended at the beginning. No task is active, see before. */
    for(idxClass=0; idxClass<RTOS_NO_PRIO_CLASSES; ++idxClass)
        _noDueTasksAry[idxClass] = 0;
    _activeTaskId = IDLE_TASK_ID;
    _suspendedTaskId = IDLE_TASK_ID;

    /* All data is prepared. Let's start the IRQ which clocks the system time. */
    rtos_enableIRQTimerTic();
    
    /* Call the application to let it configure its interrupt sources. */
#if RTOS_USE_APPL_INTERRUPT_00 == RTOS_FEATURE_ON
    rtos_enableIRQUser00();
#endif
#if RTOS_USE_APPL_INTERRUPT_01 == RTOS_FEATURE_ON
    rtos_enableIRQUser01();
#endif

    /* From here, all further code implicitly becomes the idle task. */
    while(true)
        loop();

} /* End of rtos_initRTOS */


/*
TODO: implement waitForEventsUntillTime
              , EnterLeaveCriticalSection (cli/sei and only TIMER2)
              , get/setRoundRobin?
*/