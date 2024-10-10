/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>
#include <string.h>

/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define PRIORITY_LEDS 7
#define PRIORITY_UART 1
#define PRIORITY_INIT 0

/* Events */
#define EVENT_INIT_DONE 1
#define EVENT_LED1_ON   2

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)

#if !DT_NODE_HAS_STATUS(LED0_NODE, okay)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS(LED1_NODE, okay)
#error "Unsupported board: led1 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS(LED2_NODE, okay)
#error "Unsupported board: led2 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS(LED3_NODE, okay)
#error "Unsupported board: led3 devicetree alias is not defined"
#endif

struct printk_data_t {
	void *fifo_reserved; /* 1st word reserved for use by fifo */
	uint32_t led;
	uint32_t cnt;
};

K_FIFO_DEFINE(printk_fifo);
K_EVENT_DEFINE(events)

struct led {
	struct gpio_dt_spec spec;
	uint8_t num;
};

static const struct led led0 = {
	.spec = GPIO_DT_SPEC_GET_OR(LED0_NODE, gpios, {0}),
	.num = 0,
};

static const struct led led1 = {
	.spec = GPIO_DT_SPEC_GET_OR(LED1_NODE, gpios, {0}),
	.num = 1,
};

static const struct led led2 = {
	.spec = GPIO_DT_SPEC_GET_OR(LED2_NODE, gpios, {0}),
	.num = 2,
};

static const struct led led3 = {
	.spec = GPIO_DT_SPEC_GET_OR(LED3_NODE, gpios, {0}),
	.num = 3,
};

void init()
{
	struct led leds[] = {led0, led1, led2, led3};
	for (uint8_t i = 0; i < 4; i++) {
		const struct gpio_dt_spec *spec = &(leds[i].spec);
		if (!device_is_ready(spec->port)) {
			printk("Error: %s device is not ready\n", spec->port->name);
			return;
		}
		uint8_t ret = gpio_pin_configure_dt(spec, GPIO_OUTPUT);
		if (ret != 0) {
			printk("Error %d: failed to configure pin %d (LED '%d')\n", ret, spec->pin,
			       leds[i].num);
			return;
		}
		gpio_pin_set(spec->port, spec->pin, true);
		k_msleep(200);
	}
	k_msleep(500);

	for (int8_t i = 3; i >= 0; i--) {
		gpio_pin_set(leds[i].spec.port, leds[i].spec.pin, false);
		k_msleep(200);
	}
	k_msleep(500);

	// All tasks will wait until the INIT_DONE event is set. `gpio_pin_set`
	// above demonstrates that `init` has exclusive control until freeing the other tasks.
	k_event_set(&events, EVENT_INIT_DONE);
}
/* This version of blink() never invokes the kernel, so never has yield points. */
void blink_noyield(const struct led *led, uint32_t sleep_ms, uint32_t id)
{
	int cnt = 0;

	k_event_wait(&events, EVENT_INIT_DONE, false, K_FOREVER);

	while (1) {
		gpio_pin_set(led->spec.port, led->spec.pin, cnt % 2);
		cnt++;
	}
}

/* This version of blink() uses kernel sleeps to allow other tasks to perform work. */
void blink(const struct led *led, uint32_t sleep_ms, uint32_t id)
{
	int cnt = 0;

	k_event_wait(&events, EVENT_INIT_DONE, false, K_FOREVER);

	while (1) {
		// Publish the state of LED1 as an event. Using _masked ensures that EVENT_INIT_DONE remains set.
		if (led->num == led1.num) {
			k_event_set_masked(&events, (cnt % 2) ? EVENT_LED1_ON : 0, EVENT_LED1_ON);
		}

		gpio_pin_set(led->spec.port, led->spec.pin, cnt % 2);

		struct printk_data_t *tx_data =
			(struct printk_data_t *)k_malloc(sizeof(struct printk_data_t));
		__ASSERT_NO_MSG(mem_ptr != 0);
		tx_data->led = id;
		tx_data->cnt = cnt;
		k_fifo_put(&printk_fifo, tx_data);

		k_msleep(sleep_ms);
		cnt++;
	}
}

/* This version of blink() only runs when an event is set. */
void blink_event(const struct led *led, uint32_t sleep_ms, uint32_t id)
{
	int cnt = 0;

	k_event_wait(&events, EVENT_INIT_DONE, false, K_FOREVER);
	k_event_wait(&events, EVENT_LED1_ON, false, K_FOREVER);

	while (1) {
		// If reset=false, LED2 will blink as long as LED1 is on.
		// If reset=true, LED2 will blink once when LED1 turns on.
		// (reset clears all events, including EVENT_INIT. That's fine as long as all tasks are running.)
		// Both could be useful for signalling, depending whether you want an event
		// to represent a transient state or a barrier to unblock a bunch of tasks
		// in a synchronized way. 
		// k_event_wait(&events, EVENT_LED1_ON, false, K_FOREVER);
		if (cnt % 2) {
			k_event_wait(&events, EVENT_LED1_ON, true, K_FOREVER);
		}

		gpio_pin_set(led->spec.port, led->spec.pin, cnt % 2);

		struct printk_data_t *tx_data =
			(struct printk_data_t *)k_malloc(sizeof(struct printk_data_t));
		__ASSERT_NO_MSG(mem_ptr != 0);
		tx_data->led = id;
		tx_data->cnt = cnt;
		k_fifo_put(&printk_fifo, tx_data);

		k_msleep(sleep_ms);
		cnt++;
	}
}

/* Helper function to pass arguments to blink(). Especially useful if a thread needs more than three
 * arguments */
void blink0(void)
{
	blink(&led0, 100, 0);
}

/* UART helper. Separating UART into a separate task allows printk() to run at higher or lower
 * priority, as desired. */
void uart_out(void)
{
	while (1) {
		struct printk_data_t *rx_data = k_fifo_get(&printk_fifo, K_FOREVER);
		printk("Toggled led%d; counter=%d\n", rx_data->led, rx_data->cnt);
		k_free(rx_data);
	}
}

// Initialization
K_THREAD_DEFINE(init_id, STACKSIZE, init, NULL, NULL, NULL, PRIORITY_INIT, 0, 0);
K_THREAD_DEFINE(uart_out_id, STACKSIZE, uart_out, NULL, NULL, NULL, PRIORITY_UART, 0, 0);

// Use a helper function to start a thread
K_THREAD_DEFINE(blink0_id, STACKSIZE, blink0, NULL, NULL, NULL, PRIORITY_LEDS, 0, 0);
// Start a thread with arguments and a delay
K_THREAD_DEFINE(blink1_id, STACKSIZE, blink, &led1, 1000, 1, PRIORITY_LEDS, 0, 5000);

// blink_event uses Event messaging to blink when LED1 is on.
K_THREAD_DEFINE(blink2_id, STACKSIZE, blink_event, &led2, 200, 2, PRIORITY_LEDS, 0, 0);

// The following examples use LED3 to demonstrate task blocking and prioritization.

// ========================================================
// Priority and preemption examples

// High-priority busy thread
// A delay of 0 means this thread never sleeps. When thread priority > PRIORITY, Zephyr will only
// ever run this thread.
// K_THREAD_DEFINE(blink3_id, STACKSIZE, blink, &led3, 0, 3, PRIORITY_LEDS-1, 0, 0);

// If priority is the same as peer threads, Zephyr will rotate the busy thread out when it yields or
// sleeps. (specifically, Zephyr rotates round-robin between same-priority threads) Provided a
// thread yields often, it won't disrupt other threads doing light work.
// K_THREAD_DEFINE(blink3_id, STACKSIZE, blink, &led3, 0, 3, PRIORITY_LEDS, 0, 0);

// If a busy thread is lower priority than others, Zephyr will automatically swap it out when higher
// priority tasks become ready. The low priority thread doesn't need to yield or sleep; Zephyr will
// notice the higher priority thread is ready on a system tick. If the other tasks were using more
// CPU time LED3 would stop blinking whenever another task had importatnt work to do. (you can
// probably see the UART task interrupting LED3's blinking with an oscilloscope)
// K_THREAD_DEFINE(blink3_id, STACKSIZE, blink, &led3, 0, 3, PRIORITY_LEDS+1, 0, 0);

// Zephyr is preemptive. It'll swap out a low priority thread even if the thread never yields or
// invokes the kernel.
K_THREAD_DEFINE(blink3_id, STACKSIZE, blink_noyield, &led3, 1000, 3, PRIORITY_LEDS + 1, 0, 0);

// But it won't swap equal priority threads. If a non-yielding or long-running thread is the same
// priority as others, Zephyr will let it run forever.
// K_THREAD_DEFINE(blink3_id, STACKSIZE, blink_noyield, &led3, 1000, 3, PRIORITY_LEDS, 0, 0);

// Thread options are documented here. There aren't many choices:
// - save & restore FP registers
// - restart the system if a thread exits (like a watchdog)
// Threads must be allocated statically (with the macro above), dynamic thread creation is
// unsupported as of Zephyr 3.4.0
//
// https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/kernel/services/threads/index.html#thread-options