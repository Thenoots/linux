/**
 * core.c - DesignWare USB3 DRD Controller Core file
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 * All rights reserved.
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>

#include "core.h"
#include "gadget.h"
#include "io.h"

#ifdef CONFIG_PM
static int dwc3_suspend(struct device *dev)
{
	return pm_runtime_put_sync(dev);
}

static int dwc3_resume(struct device *dev)
{
	return pm_runtime_get_sync(dev);
}

static const struct dev_pm_ops dwc3_pm_ops = {
	.suspend	= dwc3_suspend,
	.resume		= dwc3_resume,
};

#define DEV_PM_OPS	(&dwc3_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif

/**
 * dwc3_free_one_event_buffer - Frees one event buffer
 * @dwc: Pointer to our controller context structure
 * @evt: Pointer to event buffer to be freed
 */
static void dwc3_free_one_event_buffer(struct dwc3 *dwc,
		struct dwc3_event_buffer *evt)
{
	dma_free_coherent(dwc->dev, evt->length, evt->buf, evt->dma);
	kfree(evt);
}

/**
 * dwc3_alloc_one_event_buffer - Allocated one event buffer structure
 * @dwc: Pointer to our controller context structure
 * @length: size of the event buffer
 *
 * Returns a pointer to the allocated event buffer structure on succes
 * otherwise ERR_PTR(errno).
 */
static struct dwc3_event_buffer *__devinit
dwc3_alloc_one_event_buffer(struct dwc3 *dwc, unsigned length)
{
	struct dwc3_event_buffer	*evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return ERR_PTR(-ENOMEM);

	evt->dwc	= dwc;
	evt->length	= length;
	evt->buf	= dma_alloc_coherent(dwc->dev, length,
			&evt->dma, GFP_KERNEL);
	if (!evt->buf) {
		kfree(evt);
		return ERR_PTR(-ENOMEM);
	}

	return evt;
}

/**
 * dwc3_free_event_buffers - frees all allocated event buffers
 * @dwc: Pointer to our controller context structure
 */
static void dwc3_free_event_buffers(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;
	int i;

	for (i = 0; i < DWC3_EVENT_BUFFERS_NUM; i++) {
		evt = dwc->ev_buffs[i];
		if (evt) {
			dwc3_free_one_event_buffer(dwc, evt);
			dwc->ev_buffs[i] = NULL;
		}
	}
}

/**
 * dwc3_alloc_event_buffers - Allocates @num event buffers of size @length
 * @dwc: Pointer to out controller context structure
 * @num: number of event buffers to allocate
 * @length: size of event buffer
 *
 * Returns 0 on success otherwise negative errno. In error the case, dwc
 * may contain some buffers allocated but not all which were requested.
 */
static int __devinit dwc3_alloc_event_buffers(struct dwc3 *dwc, unsigned num,
		unsigned length)
{
	int			i;

	for (i = 0; i < num; i++) {
		struct dwc3_event_buffer	*evt;

		evt = dwc3_alloc_one_event_buffer(dwc, length);
		if (IS_ERR(evt)) {
			dev_err(dwc->dev, "can't allocate event buffer\n");
			return PTR_ERR(evt);
		}
		dwc->ev_buffs[i] = evt;
	}

	return 0;
}

/**
 * dwc3_event_buffers_setup - setup our allocated event buffers
 * @dwc: Pointer to out controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
static int __devinit dwc3_event_buffers_setup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;
	int				n;

	for (n = 0; n < DWC3_EVENT_BUFFERS_NUM; n++) {
		evt = dwc->ev_buffs[n];
		dev_dbg(dwc->dev, "Event buf %p dma %u length %d\n",
				evt->buf, evt->dma, evt->length);

		dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(n), evt->dma);
		dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(n), 0);
		dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(n),
				evt->length & 0xffff);
		dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(n), 0);
	}

	return 0;
}

static void dwc3_event_buffers_cleanup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;
	int				n;

	for (n = 0; n < DWC3_EVENT_BUFFERS_NUM; n++) {
		evt = dwc->ev_buffs[n];
		dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(n), 0);
		dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(n), 0);
		dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(n), 0);
		dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(n), 0);
	}
}

/**
 * dwc3_core_init - Low-level initialization of DWC3 Core
 * @dwc: Pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
static int __devinit dwc3_core_init(struct dwc3 *dwc)
{
	unsigned long		timeout = jiffies_to_msecs(jiffies + 500);
	u32			reg;

	int			ret;

	reg = dwc3_readl(dwc->regs, DWC3_GSNPSID);
	/* This should read as U3 followed by revision number */
	if ((reg & DWC3_GSNPSID_MASK) != 0x55330000) {
		dev_err(dwc->dev, "this is not a DesignWare USB3 DRD Core\n");
		ret = -ENODEV;
		goto err0;
	}

	dwc->revision = reg & DWC3_GSNPSREV_MASK;

	dwc3_writel(dwc->regs, DWC3_DCTL, DWC3_DCTL_CSFTRST);

	do {
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);

		if (time_after(jiffies, timeout)) {
			dev_err(dwc->dev, "Reset Timed Out\n");
			ret = -ETIMEDOUT;
			goto err0;
		}

		cpu_relax();
	} while (reg & DWC3_DCTL_CSFTRST);

	ret = dwc3_alloc_event_buffers(dwc, DWC3_EVENT_BUFFERS_NUM,
			DWC3_EVENT_BUFFERS_SIZE);
	if (ret) {
		dev_err(dwc->dev, "failed to allocate event buffers\n");
		ret = -ENOMEM;
		goto err1;
	}

	ret = dwc3_event_buffers_setup(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to setup event buffers\n");
		goto err1;
	}

	return 0;

err1:
	dwc3_free_event_buffers(dwc);

err0:
	return ret;
}

static void dwc3_core_exit(struct dwc3 *dwc)
{
	dwc3_event_buffers_cleanup(dwc);
	dwc3_free_event_buffers(dwc);
}

#define DWC3_ALIGN_MASK		(16 - 1)

static int __devinit dwc3_probe(struct platform_device *pdev)
{
	const struct platform_device_id *id = platform_get_device_id(pdev);
	struct resource		*res;
	struct dwc3		*dwc;
	void __iomem		*regs;
	unsigned int		features = id->driver_data;
	int			ret = -ENOMEM;
	int			irq;
	void			*mem;

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_forbid(&pdev->dev);

	mem = kzalloc(sizeof(*dwc) + DWC3_ALIGN_MASK, GFP_KERNEL);
	if (!mem) {
		dev_err(&pdev->dev, "not enough memory\n");
		goto err0;
	}
	dwc = PTR_ALIGN(mem, DWC3_ALIGN_MASK + 1);
	dwc->mem = mem;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing resource\n");
		goto err1;
	}

	regs = ioremap(res->start, resource_size(res));
	if (!regs) {
		dev_err(&pdev->dev, "ioremap failed\n");
		goto err1;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "missing IRQ\n");
		goto err2;
	}

	spin_lock_init(&dwc->lock);
	platform_set_drvdata(pdev, dwc);

	dwc->regs	= regs;
	dwc->dev	= &pdev->dev;
	dwc->irq	= irq;

	ret = dwc3_core_init(dwc);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialize core\n");
		goto err2;
	}

	if (features & DWC3_HAS_PERIPHERAL) {
		ret = dwc3_gadget_init(dwc);
		if (ret) {
			dev_err(&pdev->dev, "failed to initialized gadget\n");
			goto err3;
		}
	}

	pm_runtime_allow(&pdev->dev);

	return 0;

err3:
	dwc3_core_exit(dwc);

err2:
	iounmap(regs);

err1:
	kfree(dwc->mem);

err0:
	return ret;
}

static int __devexit dwc3_remove(struct platform_device *pdev)
{
	const struct platform_device_id *id = platform_get_device_id(pdev);
	struct dwc3	*dwc = platform_get_drvdata(pdev);
	unsigned int	features = id->driver_data;

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	if (features & DWC3_HAS_PERIPHERAL)
		dwc3_gadget_exit(dwc);

	dwc3_core_exit(dwc);
	iounmap(dwc->regs);
	kfree(dwc->mem);

	return 0;
}

static const struct platform_device_id dwc3_id_table[] = {
	{
		.name	= "omap-dwc3",
		.driver_data = (DWC3_HAS_PERIPHERAL
			| DWC3_HAS_XHCI
			| DWC3_HAS_OTG),
	},
	{
		.name	= "haps-dwc3",
		.driver_data = DWC3_HAS_PERIPHERAL,
	},
	{  },	/* Terminating Entry */
};
MODULE_DEVICE_TABLE(platform, dwc3_id_table);

static struct platform_driver dwc3_driver = {
	.probe		= dwc3_probe,
	.remove		= __devexit_p(dwc3_remove),
	.driver		= {
		.name	= "dwc3",
		.pm	= DEV_PM_OPS,
	},
	.id_table	= dwc3_id_table,
};

MODULE_AUTHOR("Felipe Balbi <balbi@ti.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("DesignWare USB3 DRD Controller Driver");

static int __init dwc3_init(void)
{
	return platform_driver_register(&dwc3_driver);
}
module_init(dwc3_init);

static void __exit dwc3_exit(void)
{
	platform_driver_unregister(&dwc3_driver);
}
module_exit(dwc3_exit);
