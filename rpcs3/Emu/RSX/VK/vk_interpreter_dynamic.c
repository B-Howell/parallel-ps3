/* See vk_interpreter_dynamic.h */

#include "vk_interpreter_dynamic.h"

#include <string.h>

unsigned vk_idyn_usable(unsigned caps, const struct vk_idyn_fns *f)
{
   if (!f->cull_mode || !f->front_face || !f->topology || !f->depth_test
         || !f->depth_write || !f->depth_compare || !f->depth_bounds_test
         || !f->stencil_test || !f->stencil_op || !f->depth_bias
         || !f->primitive_restart || !f->rasterizer_discard)
      return 0;

   if (!f->logic_op)
      caps &= ~VK_IDYN_LOGIC_OP;
   if (!f->blend_enable || !f->blend_equation || !f->write_mask)
      caps &= ~VK_IDYN_BLEND;
   if (!f->logic_op_enable)
      caps &= ~VK_IDYN_LOGIC_OP_ON;
   if (!f->depth_clamp)
      caps &= ~VK_IDYN_DEPTH_CLAMP;

   /* Each of these is only of use with the base set */
   if (!(caps & VK_IDYN_BASE))
      return 0;

   return caps;
}

/* One topology standing for its class: a pipeline made with it takes any
 * of the class when the topology is dynamic */
static VkPrimitiveTopology vk_idyn_topology_class(VkPrimitiveTopology t)
{
   switch (t)
   {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
         return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
         return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
         return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      default:
         return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   }
}

static void vk_idyn_stencil_canonical(VkStencilOpState *op, int ops)
{
   /* Dynamic in every interpreter pipeline */
   op->compareMask = 0;
   op->writeMask   = 0;
   op->reference   = 0;

   if (ops)
   {
      op->failOp      = VK_STENCIL_OP_KEEP;
      op->passOp      = VK_STENCIL_OP_KEEP;
      op->depthFailOp = VK_STENCIL_OP_KEEP;
      op->compareOp   = VK_COMPARE_OP_ALWAYS;
   }
}

void vk_idyn_canonical(unsigned caps, const struct vk_idyn_state *s)
{
   const int base = (caps & VK_IDYN_BASE) != 0;
   unsigned i;

   /* Dynamic in every interpreter pipeline */
   s->rs->lineWidth               = 1.0f;
   s->rs->depthBiasConstantFactor = 0.0f;
   s->rs->depthBiasClamp          = 0.0f;
   s->rs->depthBiasSlopeFactor    = 0.0f;
   s->ds->minDepthBounds          = 0.0f;
   s->ds->maxDepthBounds          = 0.0f;
   memset(s->cs->blendConstants, 0, sizeof(s->cs->blendConstants));
   vk_idyn_stencil_canonical(&s->ds->front, base);
   vk_idyn_stencil_canonical(&s->ds->back,  base);

   if (base)
   {
      s->ia->topology = (caps & VK_IDYN_TOPOLOGY_ANY)
         ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
         : vk_idyn_topology_class(s->ia->topology);
      s->ia->primitiveRestartEnable  = VK_FALSE;

      s->rs->cullMode                = VK_CULL_MODE_NONE;
      s->rs->frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      s->rs->depthBiasEnable         = VK_FALSE;
      s->rs->rasterizerDiscardEnable = VK_FALSE;

      s->ds->depthTestEnable         = VK_FALSE;
      s->ds->depthWriteEnable        = VK_FALSE;
      s->ds->depthCompareOp          = VK_COMPARE_OP_NEVER;
      s->ds->depthBoundsTestEnable   = VK_FALSE;
      s->ds->stencilTestEnable       = VK_FALSE;
   }

   if (caps & VK_IDYN_LOGIC_OP)
      s->cs->logicOp = VK_LOGIC_OP_CLEAR;
   if (caps & VK_IDYN_LOGIC_OP_ON)
      s->cs->logicOpEnable = VK_FALSE;
   if (caps & VK_IDYN_DEPTH_CLAMP)
      s->rs->depthClampEnable = VK_FALSE;

   if (caps & VK_IDYN_BLEND)
   {
      for (i = 0; i < s->cs->attachmentCount; i++)
      {
         VkPipelineColorBlendAttachmentState *a = &s->att[i];
         a->blendEnable         = VK_FALSE;
         a->srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
         a->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
         a->colorBlendOp        = VK_BLEND_OP_ADD;
         a->srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
         a->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
         a->alphaBlendOp        = VK_BLEND_OP_ADD;
         a->colorWriteMask      = 0;
      }
   }
}

unsigned vk_idyn_states(unsigned caps, VkDynamicState *out)
{
   unsigned n = 0;

   if (!(caps & VK_IDYN_BASE))
      return 0;

   out[n++] = VK_DYNAMIC_STATE_CULL_MODE;
   out[n++] = VK_DYNAMIC_STATE_FRONT_FACE;
   out[n++] = VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY;
   out[n++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
   out[n++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
   out[n++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
   out[n++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE;
   out[n++] = VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE;
   out[n++] = VK_DYNAMIC_STATE_STENCIL_OP;
   out[n++] = VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE;
   out[n++] = VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE;
   out[n++] = VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE;

   if (caps & VK_IDYN_LOGIC_OP)
      out[n++] = VK_DYNAMIC_STATE_LOGIC_OP_EXT;
   if (caps & VK_IDYN_BLEND)
   {
      out[n++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
      out[n++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
      out[n++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
   }
   if (caps & VK_IDYN_LOGIC_OP_ON)
      out[n++] = VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT;
   if (caps & VK_IDYN_DEPTH_CLAMP)
      out[n++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;

   return n;
}

void vk_idyn_record(const struct vk_idyn_fns *f, VkCommandBuffer cmd,
      unsigned caps, const struct vk_idyn_state *s)
{
   const VkPipelineDepthStencilStateCreateInfo *ds = s->ds;

   if (!(caps & VK_IDYN_BASE))
      return;

   f->cull_mode(cmd, s->rs->cullMode);
   f->front_face(cmd, s->rs->frontFace);
   f->topology(cmd, s->ia->topology);
   f->primitive_restart(cmd, s->ia->primitiveRestartEnable);
   f->depth_bias(cmd, s->rs->depthBiasEnable);
   f->rasterizer_discard(cmd, s->rs->rasterizerDiscardEnable);
   f->depth_test(cmd, ds->depthTestEnable);
   f->depth_write(cmd, ds->depthWriteEnable);
   f->depth_compare(cmd, ds->depthCompareOp);
   f->depth_bounds_test(cmd, ds->depthBoundsTestEnable);
   f->stencil_test(cmd, ds->stencilTestEnable);
   f->stencil_op(cmd, VK_STENCIL_FACE_FRONT_BIT, ds->front.failOp,
         ds->front.passOp, ds->front.depthFailOp, ds->front.compareOp);
   f->stencil_op(cmd, VK_STENCIL_FACE_BACK_BIT, ds->back.failOp,
         ds->back.passOp, ds->back.depthFailOp, ds->back.compareOp);

   if (caps & VK_IDYN_LOGIC_OP)
      f->logic_op(cmd, s->cs->logicOp);
   if (caps & VK_IDYN_LOGIC_OP_ON)
      f->logic_op_enable(cmd, s->cs->logicOpEnable);
   if (caps & VK_IDYN_DEPTH_CLAMP)
      f->depth_clamp(cmd, s->rs->depthClampEnable);

   if ((caps & VK_IDYN_BLEND) && s->cs->attachmentCount)
   {
      VkBool32                enable[8];
      VkColorBlendEquationEXT equation[8];
      VkColorComponentFlags   mask[8];
      unsigned                count = s->cs->attachmentCount;
      unsigned                i;

      if (count > 8)
         count = 8;

      for (i = 0; i < count; i++)
      {
         const VkPipelineColorBlendAttachmentState *a = &s->att[i];
         enable[i]                       = a->blendEnable;
         equation[i].srcColorBlendFactor = a->srcColorBlendFactor;
         equation[i].dstColorBlendFactor = a->dstColorBlendFactor;
         equation[i].colorBlendOp        = a->colorBlendOp;
         equation[i].srcAlphaBlendFactor = a->srcAlphaBlendFactor;
         equation[i].dstAlphaBlendFactor = a->dstAlphaBlendFactor;
         equation[i].alphaBlendOp        = a->alphaBlendOp;
         mask[i]                         = a->colorWriteMask;
      }

      f->blend_enable(cmd, 0, count, enable);
      f->blend_equation(cmd, 0, count, equation);
      f->write_mask(cmd, 0, count, mask);
   }
}
