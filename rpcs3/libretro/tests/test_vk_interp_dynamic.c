/* The shader interpreter's pipelines with their fixed-function state made
 * dynamic (Emu/RSX/VK/vk_interpreter_dynamic.c): two draws' states that
 * differ only in what is dynamic key one pipeline, and recording gives the
 * command buffer each draw's own values. C89. */

#include "../../Emu/RSX/VK/vk_interpreter_dynamic.h"
#include "test_common.h"

/* What the fake command buffer was given */
static struct
{
   VkCullModeFlags cull;
   VkFrontFace front;
   VkPrimitiveTopology topology;
   VkBool32 depth_test, depth_write, bounds, stencil, bias, restart, discard;
   VkCompareOp depth_op;
   VkStencilOp front_fail, back_pass;
   VkCompareOp back_compare;
   VkLogicOp logic;
   VkBool32 logic_on, clamp;
   VkBool32 blend[4];
   VkColorBlendEquationEXT eq[4];
   VkColorComponentFlags mask[4];
   unsigned blend_count;
   unsigned calls;
} got;

static void VKAPI_CALL f_cull(VkCommandBuffer c, VkCullModeFlags v) { (void)c; got.cull = v; got.calls++; }
static void VKAPI_CALL f_front(VkCommandBuffer c, VkFrontFace v) { (void)c; got.front = v; got.calls++; }
static void VKAPI_CALL f_topo(VkCommandBuffer c, VkPrimitiveTopology v) { (void)c; got.topology = v; got.calls++; }
static void VKAPI_CALL f_dtest(VkCommandBuffer c, VkBool32 v) { (void)c; got.depth_test = v; got.calls++; }
static void VKAPI_CALL f_dwrite(VkCommandBuffer c, VkBool32 v) { (void)c; got.depth_write = v; got.calls++; }
static void VKAPI_CALL f_dop(VkCommandBuffer c, VkCompareOp v) { (void)c; got.depth_op = v; got.calls++; }
static void VKAPI_CALL f_bounds(VkCommandBuffer c, VkBool32 v) { (void)c; got.bounds = v; got.calls++; }
static void VKAPI_CALL f_stest(VkCommandBuffer c, VkBool32 v) { (void)c; got.stencil = v; got.calls++; }
static void VKAPI_CALL f_sop(VkCommandBuffer c, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass, VkStencilOp zfail, VkCompareOp cmp)
{
   (void)c; (void)zfail;
   if (face & VK_STENCIL_FACE_FRONT_BIT) got.front_fail = fail;
   if (face & VK_STENCIL_FACE_BACK_BIT) { got.back_pass = pass; got.back_compare = cmp; }
   got.calls++;
}
static void VKAPI_CALL f_bias(VkCommandBuffer c, VkBool32 v) { (void)c; got.bias = v; got.calls++; }
static void VKAPI_CALL f_restart(VkCommandBuffer c, VkBool32 v) { (void)c; got.restart = v; got.calls++; }
static void VKAPI_CALL f_discard(VkCommandBuffer c, VkBool32 v) { (void)c; got.discard = v; got.calls++; }
static void VKAPI_CALL f_logic(VkCommandBuffer c, VkLogicOp v) { (void)c; got.logic = v; got.calls++; }
static void VKAPI_CALL f_blend(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBool32 *v)
{ (void)c; memcpy(got.blend + first, v, n * sizeof(*v)); got.blend_count = n; got.calls++; }
static void VKAPI_CALL f_eq(VkCommandBuffer c, uint32_t first, uint32_t n, const VkColorBlendEquationEXT *v)
{ (void)c; memcpy(got.eq + first, v, n * sizeof(*v)); got.calls++; }
static void VKAPI_CALL f_mask(VkCommandBuffer c, uint32_t first, uint32_t n, const VkColorComponentFlags *v)
{ (void)c; memcpy(got.mask + first, v, n * sizeof(*v)); got.calls++; }
static void VKAPI_CALL f_logic_on(VkCommandBuffer c, VkBool32 v) { (void)c; got.logic_on = v; got.calls++; }
static void VKAPI_CALL f_clamp(VkCommandBuffer c, VkBool32 v) { (void)c; got.clamp = v; got.calls++; }

static const struct vk_idyn_fns fns =
{
   f_cull, f_front, f_topo, f_dtest, f_dwrite, f_dop, f_bounds, f_stest, f_sop,
   f_bias, f_restart, f_discard, f_logic, f_blend, f_eq, f_mask, f_logic_on, f_clamp
};

struct pipe
{
   VkPipelineInputAssemblyStateCreateInfo ia;
   VkPipelineRasterizationStateCreateInfo rs;
   VkPipelineDepthStencilStateCreateInfo ds;
   VkPipelineColorBlendStateCreateInfo cs;
   VkPipelineColorBlendAttachmentState att[4];
};

static struct vk_idyn_state view(struct pipe *p)
{
   struct vk_idyn_state s;
   s.ia = &p->ia; s.rs = &p->rs; s.ds = &p->ds; s.cs = &p->cs; s.att = p->att;
   return s;
}

/* A shadow pass: front culled, depth tested less-equal and written,
 * two-sided stencil, no blending, colour writes off */
static void shadow(struct pipe *p)
{
   memset(p, 0, sizeof(*p));
   p->ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
   p->ia.primitiveRestartEnable = VK_TRUE;
   p->rs.cullMode = VK_CULL_MODE_FRONT_BIT;
   p->rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
   p->rs.depthBiasEnable = VK_TRUE;
   p->rs.depthClampEnable = VK_TRUE;
   p->rs.lineWidth = 2.0f;
   p->ds.depthTestEnable = VK_TRUE;
   p->ds.depthWriteEnable = VK_TRUE;
   p->ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
   p->ds.stencilTestEnable = VK_TRUE;
   p->ds.front.failOp = VK_STENCIL_OP_REPLACE;
   p->ds.front.compareMask = 0xff;
   p->ds.back.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
   p->ds.back.compareOp = VK_COMPARE_OP_EQUAL;
   p->cs.attachmentCount = 2;
   p->cs.logicOpEnable = VK_TRUE;
   p->cs.logicOp = VK_LOGIC_OP_XOR;
   p->cs.blendConstants[2] = 0.5f;
}

/* A UI pass: no culling, no depth, alpha blended lines */
static void ui(struct pipe *p)
{
   memset(p, 0, sizeof(*p));
   p->ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
   p->rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   p->rs.lineWidth = 1.0f;
   p->cs.attachmentCount = 2;
   p->att[0].blendEnable = VK_TRUE;
   p->att[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
   p->att[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   p->att[0].colorWriteMask = 0xf;
   p->att[1].colorWriteMask = 0x7;
}

static int same(const struct pipe *a, const struct pipe *b)
{
   return !memcmp(&a->ia, &b->ia, sizeof(a->ia)) && !memcmp(&a->rs, &b->rs, sizeof(a->rs))
      && !memcmp(&a->ds, &b->ds, sizeof(a->ds))
      && a->cs.attachmentCount == b->cs.attachmentCount && a->cs.logicOp == b->cs.logicOp
      && a->cs.logicOpEnable == b->cs.logicOpEnable
      && !memcmp(a->cs.blendConstants, b->cs.blendConstants, sizeof(a->cs.blendConstants))
      && !memcmp(a->att, b->att, a->cs.attachmentCount * sizeof(a->att[0]));
}

int main(void)
{
   const unsigned all = VK_IDYN_BASE | VK_IDYN_TOPOLOGY_ANY | VK_IDYN_LOGIC_OP
      | VK_IDYN_BLEND | VK_IDYN_LOGIC_OP_ON | VK_IDYN_DEPTH_CLAMP;
   struct pipe a, b, a_key, b_key;
   struct vk_idyn_state s;
   VkDynamicState states[VK_IDYN_MAX_STATES];
   struct vk_idyn_fns partial = fns;

   /* Everything dynamic: the two passes key one pipeline */
   shadow(&a); ui(&b);
   a_key = a; b_key = b;
   s = view(&a_key); vk_idyn_canonical(all, &s);
   s = view(&b_key); vk_idyn_canonical(all, &s);
   TEST_CHECK(same(&a_key, &b_key));
   TEST_CHECK(vk_idyn_states(all, states) == 18);
   TEST_CHECK(vk_idyn_states(all, states) <= VK_IDYN_MAX_STATES);

   /* And each draw gets its own values back */
   memset(&got, 0, sizeof(got));
   s = view(&a); vk_idyn_record(&fns, (VkCommandBuffer)0, all, &s);
   TEST_CHECK(got.cull == VK_CULL_MODE_FRONT_BIT && got.front == VK_FRONT_FACE_CLOCKWISE);
   TEST_CHECK(got.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && got.restart);
   TEST_CHECK(got.depth_test && got.depth_write && got.depth_op == VK_COMPARE_OP_LESS_OR_EQUAL);
   TEST_CHECK(got.stencil && got.front_fail == VK_STENCIL_OP_REPLACE);
   TEST_CHECK(got.back_pass == VK_STENCIL_OP_INCREMENT_AND_WRAP && got.back_compare == VK_COMPARE_OP_EQUAL);
   TEST_CHECK(got.bias && got.clamp && got.logic_on && got.logic == VK_LOGIC_OP_XOR);
   TEST_CHECK(got.blend_count == 2 && !got.blend[0] && got.mask[0] == 0);
   /* Every state the pipeline takes as dynamic is set, the stencil op once a face */
   TEST_CHECK(got.calls == vk_idyn_states(all, states) + 1);

   memset(&got, 0, sizeof(got));
   s = view(&b); vk_idyn_record(&fns, (VkCommandBuffer)0, all, &s);
   TEST_CHECK(got.cull == VK_CULL_MODE_NONE && got.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
   TEST_CHECK(!got.depth_test && !got.stencil && !got.clamp && !got.logic_on);
   TEST_CHECK(got.blend[0] && !got.blend[1]);
   TEST_CHECK(got.eq[0].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
   TEST_CHECK(got.eq[0].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
   TEST_CHECK(got.mask[0] == 0xf && got.mask[1] == 0x7);

   /* Only the base set: blending stays in the key, the rest goes */
   a_key = a; b_key = b;
   s = view(&a_key); vk_idyn_canonical(VK_IDYN_BASE, &s);
   s = view(&b_key); vk_idyn_canonical(VK_IDYN_BASE, &s);
   TEST_CHECK(!same(&a_key, &b_key));
   TEST_CHECK(a_key.rs.cullMode == VK_CULL_MODE_NONE && !a_key.ds.depthTestEnable);
   /* Without any topology, only its class: strips stay apart from lines */
   TEST_CHECK(a_key.ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
   TEST_CHECK(b_key.ia.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
   TEST_CHECK(b_key.att[0].blendEnable && a_key.rs.depthClampEnable);
   memset(&got, 0, sizeof(got));
   s = view(&a); vk_idyn_record(&fns, (VkCommandBuffer)0, VK_IDYN_BASE, &s);
   TEST_CHECK(got.calls == vk_idyn_states(VK_IDYN_BASE, states) + 1 && got.blend_count == 0);

   /* Nothing dynamic: only what every interpreter pipeline has already */
   a_key = a;
   s = view(&a_key); vk_idyn_canonical(0, &s);
   TEST_CHECK(a_key.rs.cullMode == VK_CULL_MODE_FRONT_BIT && a_key.ds.depthTestEnable);
   TEST_CHECK(a_key.ds.front.failOp == VK_STENCIL_OP_REPLACE);
   TEST_CHECK(a_key.rs.lineWidth == 1.0f && a_key.cs.blendConstants[2] == 0.0f);
   TEST_CHECK(a_key.ds.front.compareMask == 0);
   TEST_CHECK(vk_idyn_states(0, states) == 0);
   memset(&got, 0, sizeof(got));
   s = view(&a); vk_idyn_record(&fns, (VkCommandBuffer)0, 0, &s);
   TEST_CHECK(got.calls == 0);

   /* A device short of functions loses what needs them */
   partial.blend_equation = NULL;
   TEST_CHECK(vk_idyn_usable(all, &partial) == (all & ~VK_IDYN_BLEND));
   partial.cull_mode = NULL;
   TEST_CHECK(vk_idyn_usable(all, &partial) == 0);
   TEST_CHECK(vk_idyn_usable(all & ~VK_IDYN_BASE, &fns) == 0);

   if (test_failures)
   {
      printf("test_vk_interp_dynamic: FAIL\n");
      return 1;
   }
   printf("test_vk_interp_dynamic: ok\n");
   return 0;
}
