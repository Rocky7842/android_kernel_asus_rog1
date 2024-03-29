/*****************************************************************************
* File: device.c
*
* (c) 2016 Sentons Inc. - All Rights Reserved.
*
* All information contained herein is and remains the property of Sentons
* Incorporated and its suppliers if any. The intellectual and technical
* concepts contained herein are proprietary to Sentons Incorporated and its
* suppliers and may be covered by U.S. and Foreign Patents, patents in
* process, and are protected by trade secret or copyright law. Dissemination
* of this information or reproduction of this material is strictly forbidden
* unless prior written permission is obtained from Sentons Incorporated.
*
* SENTONS PROVIDES THIS SOURCE CODE STRICTLY ON AN "AS IS" BASIS,
* WITHOUT ANY WARRANTY WHATSOEVER, AND EXPRESSLY DISCLAIMS ALL
* WARRANTIES, EXPRESS, IMPLIED OR STATUTORY WITH REGARD THERETO, INCLUDING
* THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
* PURPOSE, TITLE OR NON-INFRINGEMENT OF THIRD PARTY RIGHTS. SENTONS SHALL
* NOT BE LIABLE FOR ANY DAMAGES SUFFERED BY YOU AS A RESULT OF USING,
* MODIFYING OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.
*****************************************************************************/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pinctrl/consumer.h>
#include <linux/delay.h>

#include "workqueue.h"
#include "device.h"
#include "utils.h"
#include "hardware.h"
#include "config.h"
#include "debug.h"
#include "file_control.h"
#include "locking.h"


/*==========================================================================*/
/* DEFINES                                                                  */
/*==========================================================================*/

/*==========================================================================*/
/* CONSTANTS                                                                */
/*==========================================================================*/

/*==========================================================================*/
/* GLOBALS                                                                  */
/*==========================================================================*/
struct snt8100fsr *snt8100fsr_g; // The first and main device and sysFS device
struct snt8100fsr *snt8100fsr_wake_i2c_g; // The i2c wakeup device
struct grip_status *grip_status_g;
struct DPC_status *DPC_status_g;
/*==========================================================================*/
/* LOCAL PROTOTYPES                                                         */
/*==========================================================================*/
#if USE_DEVICE_TREE_CONFIG
static int snt_request_named_gpio(struct snt8100fsr *snt8100fsr,
                                  const char *label,
                                  int *gpio);
static int select_pin_ctl(struct snt8100fsr *snt8100fsr,
                          const char *name);
#endif

/*==========================================================================*/
/* GLOBAL VARIABLES                                                         */
/*==========================================================================*/

/*==========================================================================*/
/* METHODS                                                                  */
/*==========================================================================*/
/*
 * Device initialization using device tree node configuration
 */
#if USE_DEVICE_TREE_CONFIG // config.h =======================================
int snt_spi_device_init(struct spi_device *spi,
                        struct snt8100fsr *snt8100fsr)
{
    int ret = 0, data, i;
    struct device *dev = &spi->dev;
    struct device_node *np = dev->of_node;

    // Check device tree
    if (!np) {
        PRINT_ERR("no of node found");
        return -EINVAL;
    }

    // Request gpio
    snt_request_named_gpio(snt8100fsr,
                           "snt,gpio_hostirq",
                           &snt8100fsr->hostirq_gpio);
    snt_request_named_gpio(snt8100fsr,
                           "snt,gpio_wakeirq",
                           &snt8100fsr->wakeirq_gpio);
    snt_request_named_gpio(snt8100fsr,
                           "snt,gpio_rst",
                           &snt8100fsr->rst_gpio);

    ret = of_property_read_u32(np, "snt,spi-freq-khz", &data);
    if(ret < 0) {
        dev_err(dev, "snt,spi-freq-khz not found\n");
    } else {
        snt8100fsr->spi_freq_khz = data;
    }
    dev_info(dev, "spi_freq_khz %d\n", snt8100fsr->spi_freq_khz);

    // Request pinctrl
    snt8100fsr->snt_pinctrl = devm_pinctrl_get(dev);
    if(IS_ERR(snt8100fsr->snt_pinctrl)) {
        if(PTR_ERR(snt8100fsr->snt_pinctrl) == -EPROBE_DEFER)
        {
            dev_info(dev, "pinctrl not ready\n");
            return -EPROBE_DEFER;
        }
        dev_err(dev, "Target does not use pinctrl\n");
        snt8100fsr->snt_pinctrl = NULL;
        return  -EINVAL;
    }

    for(i = 0;i < ARRAY_SIZE(snt8100fsr->pinctrl_state);i++) {
        const char *n = pctl_names[i];
        struct pinctrl_state *state =
            pinctrl_lookup_state(snt8100fsr->snt_pinctrl, n);
        if (IS_ERR(state)) {
            dev_err(dev, "cannot find '%s'\n", n);
            return -EINVAL;
        }
        dev_info(dev, "found pin control %s\n", n);
        snt8100fsr->pinctrl_state[i] = state;
    }

    ret = select_pin_ctl(snt8100fsr, "snt_reset_reset");
    if(ret)
        return ret;

    ret = select_pin_ctl(snt8100fsr, "snt_hostirq_active");
    if(ret)
        return ret;

    device_init_wakeup(snt8100fsr->dev, 1);
    mutex_init(&snt8100fsr->track_report_sysfs_lock);
    mutex_init(&snt8100fsr->sb_lock);

    //ret =  devm_request_threaded_irq(dev, gpio_to_irq(snt8100fsr->hostirq_gpio),
    //    NULL, fpc1020_irq_handler, IRQF_TRIGGER_RISING | IRQF_ONESHOT | IRQF_NO_SUSPEND,
    //    dev_name(dev), snt8100fsr);

    // Request that the interrupt should be wakeable
    //enable_irq_wake(gpio_to_irq(snt8100fsr->hostirq_gpio));

    //ret = sysfs_create_group(&dev->kobj, &attribute_group);
    //if(ret)
    //    return ret;

    snt8100fsr->frame_rate = DEFAULT_FRAME_RATE;
    snt8100fsr->suspended_frame_rate = DEFAULT_SUSPENDED_FRAME_RATE;

    //FIXME Remove if no need
    dev_info(dev, "Enabling hardware\n");
    MUTEX_LOCK(&snt8100fsr->sb_lock);
    dev_info(dev, "[EDGE] %s(%d): snt8100sfr_spi_setup enable\n", __func__, __LINE__);
    (void)select_pin_ctl(snt8100fsr, "snt_reset_reset");
    usleep_range(100, 900);
    (void)select_pin_ctl(snt8100fsr, "snt_reset_active");
    usleep_range(100, 100);
    mutex_unlock(&snt8100fsr->sb_lock);

    return ret;
}

static int snt_request_named_gpio(struct snt8100fsr *snt8100fsr,
                                  const char *label,
                                  int *gpio)
{
    struct device *dev = snt8100fsr->dev;
    struct device_node *np = dev->of_node;
    int rc = of_get_named_gpio(np, label, 0);
    if (rc < 0) {
        PRINT_ERR("failed to get '%s'", label);
        return rc;
    }
    *gpio = rc;
    rc = devm_gpio_request(dev, *gpio, label);
    if (rc) {
        PRINT_ERR("failed to request gpio %d", *gpio);
        return rc;
    }
    PRINT_DEBUG("%s %d", label, *gpio);
    return 0;
}

static int select_pin_ctl(struct snt8100fsr *snt8100fsr, const char *name)
{
    size_t i;
    int ret;
    struct device *dev = snt8100fsr->dev;
    for (i = 0; i < ARRAY_SIZE(snt8100fsr->pinctrl_state); i++) {
        const char *n = pctl_names[i];
        if (!strncmp(n, name, strlen(n))) {
            ret = pinctrl_select_state(snt8100fsr->snt_pinctrl,
                    snt8100fsr->pinctrl_state[i]);
            if (ret)
                dev_err(dev, "cannot select '%s'\n", name);
            else
                dev_info(dev, "Selected '%s'\n", name);
            goto exit;
        }
    }
    ret = -EINVAL;
    dev_err(dev, "%s:'%s' not found\n", __func__, name);

exit:
    return ret;
}
#else // ---------------------------------------------------------------------
/*
 * Device initialization using the CONFIG.H file
 */
int snt_spi_device_init(struct spi_device *spi,
                        struct snt8100fsr *snt8100fsr) {
    struct device *dev = &spi->dev;

    snt8100fsr->hostirq_gpio = BEAGLEBONE_GPIO49;
    snt8100fsr->wakeirq_gpio = 0;
    snt8100fsr->rst_gpio = 0;
    snt8100fsr->spi_freq_khz = SPI_MAX_SPEED_HZ;
    snt8100fsr->frame_rate = DEFAULT_FRAME_RATE;
    snt8100fsr->suspended_frame_rate = DEFAULT_SUSPENDED_FRAME_RATE;

    dev_info(dev, "spi_freq_khz %d\n", snt8100fsr->spi_freq_khz);

    // Request pinctrl
    snt8100fsr->snt_pinctrl = NULL;

    device_init_wakeup(snt8100fsr->dev, 1);

    mutex_init(&snt8100fsr->track_report_sysfs_lock);
    mutex_init(&snt8100fsr->sb_lock);
    return 0;
}

int snt_i2c_device_init(struct i2c_client *i2c,
                        struct snt8100fsr *snt8100fsr) {
	PRINT_FUNC();
    snt8100fsr->hostirq_gpio = IRQ_GPIO;
    snt8100fsr->wakeirq_gpio = 0;
    snt8100fsr->rst_gpio = RST_GPIO;
    snt8100fsr->frame_rate = DEFAULT_FRAME_RATE;
    snt8100fsr->suspended_frame_rate = DEFAULT_SUSPENDED_FRAME_RATE;

    // Request pinctrl
    snt8100fsr->snt_pinctrl = NULL;
    snt8100fsr->driver_status = GRIP_I2C_PROBE; 
	
    device_init_wakeup(snt8100fsr->dev, 1);

    mutex_init(&snt8100fsr->track_report_sysfs_lock);
    mutex_init(&snt8100fsr->sb_lock);
    mutex_init(&snt8100fsr->ap_lock);
    mutex_init(&snt8100fsr->tap_lock);
    mutex_init(&snt8100fsr->IRQ_WAKE_SLEEP_LOCK);
	PRINT_FUNC("done");
    return 0;
}
#endif // ====================================================================


static ssize_t snt_spi_test_set(struct device *dev,
        struct device_attribute *attr, const char *buf, size_t count)
{
    int ret = 0;
    struct snt8100fsr *snt8100fsr = dev_get_drvdata(dev);

    dev_info(&snt8100fsr->spi->dev, "%s\n",__func__);

    return ret? ret:count;
}

static DEVICE_ATTR(snt_spi_test, S_IWUSR, NULL, snt_spi_test_set);

static struct attribute *attributes[] = {
    &dev_attr_snt_spi_test.attr,
    NULL
};

static const struct attribute_group attribute_group = {
    .attrs = attributes,
};

extern struct delayed_work check_suspend;
extern int SNT_SUSPEND_FLAG;
int snt_suspend(struct device *dev)
{
    //int ret;

    PRINT_FUNC();
    // We don't mutex lock here, due to write_register locking
    
    /* DPC condition */
    if(SNT_SUSPEND_FLAG == 1){
	if(grip_status_g->G_EN==1 && grip_status_g->G_DPC_STATUS==0){
		PRINT_INFO("Suspend: No DPC when grip enable?");
	}
	/*
	    if(grip_status_g->G_EN==1 && grip_status_g->G_DPC_STATUS==0){
		//check_gesture_before_suspend();
		    PRINT_INFO("write suspend rate: %d", snt8100fsr_g->suspended_frame_rate);
		    ret = write_register(snt8100fsr_g,
		                         REGISTER_FRAME_RATE,
		                         &snt8100fsr_g->suspended_frame_rate);
		    if (ret) {
		        PRINT_CRIT("write_register(REGISTER_FRAME_RATE) failed");
		    }
	    }else{
		//DPC_status_g->Condition = 0;
		//DPC_write_func();
		//Wait_Wake_For_RegW();

		PRINT_INFO("Setting frame rate to %d",
		                snt8100fsr_g->suspended_frame_rate);
		    ret = write_register(snt8100fsr_g,
		                         REGISTER_FRAME_RATE,
		                         &snt8100fsr_g->suspended_frame_rate);
		    if (ret) {
		        PRINT_CRIT("write_register(REGISTER_FRAME_RATE) failed");
		    }

	    }
	*/
    }
    PRINT_DEBUG("done");
    return 0;
}

extern int SNT_SUSPEND_FLAG;
extern int power_status;
int snt_resume(struct device *dev)
{
    //int ret;

    PRINT_FUNC();
    if(SNT_SUSPEND_FLAG == 0){
	PRINT_INFO("Grip Status: %d, %d, %d, %d, %d, %d, %d",
		grip_status_g->G_EN, grip_status_g->G_SQUEEZE_EN,
		grip_status_g->G_RAW_EN, grip_status_g->G_TAP1_EN, 
		grip_status_g->G_TAP2_EN, grip_status_g->G_TAP3_EN,
		grip_status_g->G_DPC_STATUS
		);
	/* Power Supply check */
	/*
	if(power_status == 1){
	    // We mutex lock here since we're calling sb_wake_device which never locks
	    MUTEX_LOCK(&snt8100fsr_g->sb_lock);
	    if(grip_status_g->G_EN  == 1 && grip_status_g->G_DPC_STATUS==0){
		ret = sb_wake_device(snt8100fsr_g);
		if (ret) {
	        	PRINT_CRIT("sb_wake_device() failed");
	        	mutex_unlock(&snt8100fsr_g->sb_lock);
	        	return ret;
	    	}
	    }else{
	    	PRINT_INFO("grip disable, disable sb_wake");
	    }

	    mutex_unlock(&snt8100fsr_g->sb_lock);
	}
	*/
    }
	/*
	if(SNT_SUSPEND_FLAG == 1){
	    workqueue_queue_work(&check_resume, 100);
	}*/
    PRINT_DEBUG("done");
    return 0;
}
