/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RK_V4L2_ASYNC_HELPERS_H
#define RK_V4L2_ASYNC_HELPERS_H

#include <linux/fwnode.h>
#include <media/v4l2-async.h>
#include <media/v4l2-fwnode.h>

static inline int rk_v4l2_async_nf_parse_fwnode_endpoints(
	struct device *dev, struct v4l2_async_notifier *notifier,
	unsigned int asd_struct_size,
	int (*parse_endpoint)(struct device *dev,
			      struct v4l2_fwnode_endpoint *vep,
			      struct v4l2_async_connection *asc))
{
	struct fwnode_handle *ep = NULL;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	int ret = 0;

	if (!fwnode)
		return -ENODEV;

	while ((ep = fwnode_graph_get_next_endpoint(fwnode, ep))) {
		struct v4l2_fwnode_endpoint vep = {
			.bus_type = V4L2_MBUS_UNKNOWN,
		};
		struct v4l2_async_connection *asc;

		ret = v4l2_fwnode_endpoint_alloc_parse(ep, &vep);
		if (ret)
			goto err_put_endpoint;

		asc = __v4l2_async_nf_add_fwnode_remote(notifier, ep,
							asd_struct_size);
		if (IS_ERR(asc)) {
			ret = PTR_ERR(asc);
			v4l2_fwnode_endpoint_free(&vep);
			if (ret == -EEXIST || ret == -ENOTCONN) {
				ret = 0;
				continue;
			}
			goto err_put_endpoint;
		}

		if (parse_endpoint) {
			ret = parse_endpoint(dev, &vep, asc);
			if (ret) {
				v4l2_fwnode_endpoint_free(&vep);
				goto err_put_endpoint;
			}
		}

		v4l2_fwnode_endpoint_free(&vep);
	}

	return 0;

err_put_endpoint:
	fwnode_handle_put(ep);
	return ret;
}

static inline int rk_v4l2_async_notifier_clr_unready_dev(
	struct v4l2_async_notifier *notifier)
{
	/* Best-effort no-op when core helper is unavailable. */
	return 0;
}

#endif
