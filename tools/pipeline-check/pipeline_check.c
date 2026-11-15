/* pipeline-check: hand SPIR-V modules to a Vulkan driver's shader compiler by creating one pipeline, and say whether
 * the driver took them. Nothing is drawn or submitted, so it runs against a driver on Mesa's no-op drm-shim: on the
 * shader-check job that driver is Turnip, whose pipeline creation is spirv_to_nir plus the ir3 compiler.
 *
 *   pipeline-check --vert V.spv --frag F.spv --color FORMAT [--color FORMAT ...]
 *   pipeline-check --comp K.spv
 *
 * The descriptor set layouts, the vertex attributes and the input attachments are read from the modules themselves
 * (OpDecorate DescriptorSet/Binding/Location/InputAttachmentIndex and the variables' types), so this is the modules'
 * own interface, not the layout the Metal runtime would build: a descriptor's class is right, its dynamic or inline
 * flavour is not known here. A fragment module that reads input attachments gets a render pass whose subpass lists
 * each colour attachment as the input attachment of the same index, and VK_EXT_rasterization_order_attachment_access
 * when the device has it (framebuffer fetch).
 *
 * Exit 0 and a line starting "PASS" when the driver created the pipeline, 1 and "FAIL" when it refused, 2 on a usage
 * or setup error. Ours, MIT. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define MAX_IDS 65536
#define MAX_BINDINGS 256
#define MAX_COLORS 8
#define MAX_ATTRS 32

enum { T_NONE, T_FLOAT, T_INT, T_VECTOR, T_MATRIX, T_IMAGE, T_SAMPLER, T_SAMPLED_IMAGE, T_ARRAY, T_RUNTIME_ARRAY, T_STRUCT, T_POINTER };

struct type { uint8_t kind; uint32_t a, b, c; };   /* meaning by kind, see parse() */
struct deco { int32_t set, binding, location, input_index; uint8_t block, buffer_block, builtin; };
struct binding { uint32_t set, binding, count; VkDescriptorType type; VkShaderStageFlags stages; };
struct attr { uint32_t location; VkFormat format; };

struct module {
	uint32_t *words; size_t count;
	char entry[256]; uint32_t model;
};

static struct binding bindings[MAX_BINDINGS]; static unsigned binding_count;
static struct attr attrs[MAX_ATTRS]; static unsigned attr_count;
static uint32_t input_attachment_mask; static int has_push;

static void die(const char *what) { fprintf(stderr, "pipeline-check: %s\n", what); exit(2); }

static void load(const char *path, struct module *m)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
	if (bytes < 20 || bytes % 4) die("not a SPIR-V module (size)");
	m->words = malloc((size_t)bytes); m->count = (size_t)bytes / 4;
	if (fread(m->words, 1, (size_t)bytes, f) != (size_t)bytes) die("short read");
	fclose(f);
	if (m->words[0] != 0x07230203) die("not a SPIR-V module (magic)");
}

static void add_binding(uint32_t set, uint32_t binding, uint32_t count, VkDescriptorType type, VkShaderStageFlags stage)
{
	for (unsigned i = 0; i < binding_count; i++)
		if (bindings[i].set == set && bindings[i].binding == binding) {
			bindings[i].stages |= stage;
			if (bindings[i].type != type) fprintf(stderr, "pipeline-check: set %u binding %u has two descriptor types across stages\n", set, binding);
			return;
		}
	if (binding_count == MAX_BINDINGS) die("too many bindings");
	bindings[binding_count++] = (struct binding){set, binding, count, type, stage};
}

/* Reads one module: its entry point of the wanted execution model and its interface. */
static void parse(struct module *m, uint32_t model, VkShaderStageFlags stage)
{
	static struct type types[MAX_IDS]; static struct deco decos[MAX_IDS]; static uint32_t constants[MAX_IDS];
	memset(types, 0, sizeof(types)); memset(constants, 0, sizeof(constants));
	for (unsigned i = 0; i < MAX_IDS; i++) decos[i] = (struct deco){-1, -1, -1, -1, 0, 0, 0};
	if (m->words[3] > MAX_IDS) die("module id bound too large");
	m->entry[0] = 0; m->model = model;
	for (size_t at = 5; at < m->count;) {
		uint32_t op = m->words[at] & 0xffff, n = m->words[at] >> 16; const uint32_t *w = &m->words[at];
		if (!n || at + n > m->count) die("malformed module");
		switch (op) {
		case 15: /* OpEntryPoint model id name */
			if (w[1] == model && !m->entry[0]) snprintf(m->entry, sizeof(m->entry), "%s", (const char *)&w[3]);
			break;
		case 71: /* OpDecorate target decoration [literal] */
			switch (w[2]) {
			case 2: decos[w[1]].block = 1; break;
			case 3: decos[w[1]].buffer_block = 1; break;
			case 11: decos[w[1]].builtin = 1; break;
			case 30: decos[w[1]].location = (int32_t)w[3]; break;
			case 33: decos[w[1]].binding = (int32_t)w[3]; break;
			case 34: decos[w[1]].set = (int32_t)w[3]; break;
			case 43: decos[w[1]].input_index = (int32_t)w[3]; break;
			}
			break;
		case 21: types[w[1]] = (struct type){T_INT, w[2], w[3], 0}; break;           /* width, signed */
		case 22: types[w[1]] = (struct type){T_FLOAT, w[2], 0, 0}; break;            /* width */
		case 23: types[w[1]] = (struct type){T_VECTOR, w[2], w[3], 0}; break;        /* component, count */
		case 24: types[w[1]] = (struct type){T_MATRIX, w[2], w[3], 0}; break;        /* column, count */
		case 25: types[w[1]] = (struct type){T_IMAGE, w[3], w[7], 0}; break;         /* dim, sampled */
		case 26: types[w[1]].kind = T_SAMPLER; break;
		case 27: types[w[1]] = (struct type){T_SAMPLED_IMAGE, w[2], 0, 0}; break;
		case 28: types[w[1]] = (struct type){T_ARRAY, w[2], w[3], 0}; break;         /* element, length id */
		case 29: types[w[1]] = (struct type){T_RUNTIME_ARRAY, w[2], 0, 0}; break;
		case 30: types[w[1]].kind = T_STRUCT; break;
		case 32: types[w[1]] = (struct type){T_POINTER, w[2], w[3], 0}; break;       /* storage class, pointee */
		case 43: if (n >= 4) constants[w[2]] = w[3]; break;                          /* OpConstant type id value */
		case 59: { /* OpVariable pointer-type id storage-class */
			uint32_t id = w[2], sc = w[3], t = types[w[1]].b, count = 1;
			while (types[t].kind == T_ARRAY || types[t].kind == T_RUNTIME_ARRAY) {
				if (types[t].kind == T_ARRAY) count *= constants[types[t].b] ? constants[types[t].b] : 1;
				t = types[t].a;
			}
			if (sc == 9) { has_push = 1; break; }
			if (sc == 1 && model == 0 && decos[id].location >= 0 && !decos[id].builtin) { /* a vertex attribute */
				uint32_t comps = 1, base = t;
				if (types[t].kind == T_MATRIX) base = t = types[t].a;
				if (types[t].kind == T_VECTOR) { comps = types[t].b; base = types[t].a; }
				static const VkFormat f[3][4] = {
					{VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT},
					{VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT},
					{VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT}};
				int row = types[base].kind == T_FLOAT ? 0 : types[base].b ? 1 : 2;
				if (attr_count < MAX_ATTRS && comps >= 1 && comps <= 4)
					attrs[attr_count++] = (struct attr){(uint32_t)decos[id].location, f[row][comps - 1]};
				break;
			}
			if (sc != 0 && sc != 2 && sc != 12) break;
			if (decos[id].set < 0 || decos[id].binding < 0) break;
			VkDescriptorType type;
			switch (types[t].kind) {
			case T_STRUCT:
				type = sc == 12 || decos[t].buffer_block ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				break;
			case T_SAMPLER: type = VK_DESCRIPTOR_TYPE_SAMPLER; break;
			case T_SAMPLED_IMAGE: type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
			case T_IMAGE:
				if (types[t].a == 6) {
					type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
					if (decos[id].input_index >= 0 && decos[id].input_index < 32) input_attachment_mask |= 1u << decos[id].input_index;
				} else if (types[t].a == 5)
					type = types[t].b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
				else
					type = types[t].b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				break;
			default:
				fprintf(stderr, "pipeline-check: variable %u (set %d binding %d) has a type this tool does not place\n", id, decos[id].set, decos[id].binding);
				goto next;
			}
			add_binding((uint32_t)decos[id].set, (uint32_t)decos[id].binding, count, type, stage);
			break;
		}
		case 54: at = m->count; continue; /* OpFunction: the interface is complete */
		}
next:
		at += n;
	}
	if (!m->entry[0]) die("the module has no entry point of the wanted stage");
}

static VkFormat color_format(const char *name)
{
	static const struct { const char *name; VkFormat format; } known[] = {
		{"RGBA8Unorm", VK_FORMAT_R8G8B8A8_UNORM}, {"BGRA8Unorm", VK_FORMAT_B8G8R8A8_UNORM},
		{"RGBA8Unorm_sRGB", VK_FORMAT_R8G8B8A8_SRGB}, {"BGRA8Unorm_sRGB", VK_FORMAT_B8G8R8A8_SRGB},
		{"RGBA16Float", VK_FORMAT_R16G16B16A16_SFLOAT}, {"RGBA32Float", VK_FORMAT_R32G32B32A32_SFLOAT},
		{"R8Unorm", VK_FORMAT_R8_UNORM}, {"RG8Unorm", VK_FORMAT_R8G8_UNORM}, {"R16Float", VK_FORMAT_R16_SFLOAT},
		{"RGB10A2Unorm", VK_FORMAT_A2B10G10R10_UNORM_PACK32}};
	for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (!strcmp(name, known[i].name)) return known[i].format;
	fprintf(stderr, "pipeline-check: unknown colour format %s\n", name); exit(2);
}

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { fprintf(stderr, "pipeline-check: %s = %d\n", #call, (int)r_); exit(2); } } while (0)

int main(int argc, char **argv)
{
	const char *vert = NULL, *frag = NULL, *comp = NULL; VkFormat colors[MAX_COLORS]; unsigned color_count = 0;
	for (int i = 1; i < argc; i++) {
		if (i + 1 >= argc) die("usage: pipeline-check --vert V.spv --frag F.spv --color FORMAT... | --comp K.spv");
		if (!strcmp(argv[i], "--vert")) vert = argv[++i];
		else if (!strcmp(argv[i], "--frag")) frag = argv[++i];
		else if (!strcmp(argv[i], "--comp")) comp = argv[++i];
		else if (!strcmp(argv[i], "--color")) { if (color_count == MAX_COLORS) die("too many colours"); colors[color_count++] = color_format(argv[++i]); }
		else die("unknown option");
	}
	if (!comp && (!vert || !frag || !color_count)) die("a render pipeline needs --vert, --frag and at least one --color");
	struct module vm = {0}, fm = {0}, cm = {0};
	if (comp) { load(comp, &cm); parse(&cm, 5, VK_SHADER_STAGE_COMPUTE_BIT); }
	else { load(vert, &vm); parse(&vm, 0, VK_SHADER_STAGE_VERTEX_BIT); load(frag, &fm); parse(&fm, 4, VK_SHADER_STAGE_FRAGMENT_BIT); }

	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pipeline-check", .apiVersion = VK_API_VERSION_1_3};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
	VkPhysicalDevice gpus[8]; uint32_t gpu_count = 8;
	VkResult er = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !gpu_count) die("no Vulkan device");
	VkPhysicalDevice gpu = gpus[0];
	VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu, &props);
	printf("device: %s (vendor 0x%x, device 0x%x, driver version 0x%x, API %u.%u.%u)\n", props.deviceName, props.vendorID, props.deviceID,
	       props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

	/* Every core feature the device has is enabled, so the driver's SPIR-V front end accepts what the device can run. */
	int raster_order = 0; uint32_t ext_count = 0;
	vkEnumerateDeviceExtensionProperties(gpu, NULL, &ext_count, NULL);
	VkExtensionProperties *exts = calloc(ext_count ? ext_count : 1, sizeof(*exts));
	vkEnumerateDeviceExtensionProperties(gpu, NULL, &ext_count, exts);
	for (uint32_t i = 0; i < ext_count; i++)
		if (!strcmp(exts[i].extensionName, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) raster_order = 1;
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT ro = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
	VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = raster_order ? &ro : NULL};
	VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
	VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
	VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
	if (props.apiVersion < VK_API_VERSION_1_3) die("the device is older than Vulkan 1.3");
	vkGetPhysicalDeviceFeatures2(gpu, &f2);
	f2.features.robustBufferAccess = VK_FALSE;
	raster_order = raster_order && ro.rasterizationOrderColorAttachmentAccess;
	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
	const char *enabled[1] = {VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
	                          .enabledExtensionCount = raster_order ? 1 : 0, .ppEnabledExtensionNames = enabled};
	VkDevice device; CHECK(vkCreateDevice(gpu, &dci, NULL, &device));

	/* Set layouts 0..max, each with the bindings the modules declare. */
	uint32_t set_count = 0;
	for (unsigned i = 0; i < binding_count; i++) if (bindings[i].set + 1 > set_count) set_count = bindings[i].set + 1;
	if (set_count > 32) die("too many descriptor sets");
	VkDescriptorSetLayout sets[32];
	for (uint32_t s = 0; s < set_count; s++) {
		VkDescriptorSetLayoutBinding b[MAX_BINDINGS]; uint32_t n = 0;
		for (unsigned i = 0; i < binding_count; i++)
			if (bindings[i].set == s) {
				b[n++] = (VkDescriptorSetLayoutBinding){.binding = bindings[i].binding, .descriptorType = bindings[i].type, .descriptorCount = bindings[i].count, .stageFlags = bindings[i].stages};
				printf("layout: set %u binding %u type %d count %u stages 0x%x\n", s, bindings[i].binding, (int)bindings[i].type, bindings[i].count, bindings[i].stages);
			}
		VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
		CHECK(vkCreateDescriptorSetLayout(device, &lci, NULL, &sets[s]));
	}
	VkPushConstantRange push = {.stageFlags = comp ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = props.limits.maxPushConstantsSize};
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = set_count, .pSetLayouts = sets,
	                                   .pushConstantRangeCount = has_push ? 1 : 0, .pPushConstantRanges = &push};
	VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));

	VkPipeline pipeline = VK_NULL_HANDLE; VkResult result;
	if (comp) {
		VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = cm.count * 4, .pCode = cm.words};
		VkShaderModule mod; CHECK(vkCreateShaderModule(device, &mci, NULL, &mod));
		VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
			.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = cm.entry}};
		result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline);
	} else {
		VkAttachmentDescription ad[MAX_COLORS]; VkAttachmentReference cref[MAX_COLORS], iref[32]; uint32_t input_count = 0;
		for (unsigned i = 0; i < color_count; i++) {
			ad[i] = (VkAttachmentDescription){.format = colors[i], .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
			cref[i] = (VkAttachmentReference){i, VK_IMAGE_LAYOUT_GENERAL};
		}
		for (uint32_t i = 0; i < 32; i++)
			if (input_attachment_mask >> i) {
				iref[i] = (VkAttachmentReference){(input_attachment_mask >> i & 1) && i < color_count ? i : VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
				input_count = i + 1;
			}
		VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = color_count, .pColorAttachments = cref,
		                            .inputAttachmentCount = input_count, .pInputAttachments = iref,
		                            .flags = input_count && raster_order ? VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT : 0};
		VkSubpassDependency self = {.srcSubpass = 0, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
		VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = color_count, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sub,
		                               .dependencyCount = input_count ? 1 : 0, .pDependencies = &self};
		VkRenderPass pass; CHECK(vkCreateRenderPass(device, &rpci, NULL, &pass));
		printf("render pass: %u colour attachment(s), %u input attachment reference(s), rasterization-order access %s\n", color_count, input_count, input_count && raster_order ? "on" : "off");

		VkShaderModuleCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vm.count * 4, .pCode = vm.words};
		VkShaderModuleCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fm.count * 4, .pCode = fm.words};
		VkShaderModule vmod, fmod; CHECK(vkCreateShaderModule(device, &vci, NULL, &vmod)); CHECK(vkCreateShaderModule(device, &fci, NULL, &fmod));
		VkPipelineShaderStageCreateInfo stages[2] = {
			{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vmod, .pName = vm.entry},
			{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fmod, .pName = fm.entry}};
		VkVertexInputAttributeDescription va[MAX_ATTRS];
		for (unsigned i = 0; i < attr_count; i++) va[i] = (VkVertexInputAttributeDescription){.location = attrs[i].location, .binding = 0, .format = attrs[i].format, .offset = 16 * i};
		VkVertexInputBindingDescription vb = {.binding = 0, .stride = 16 * (attr_count ? attr_count : 1), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
		VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = attr_count ? 1 : 0, .pVertexBindingDescriptions = &vb,
		                                           .vertexAttributeDescriptionCount = attr_count, .pVertexAttributeDescriptions = va};
		VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
		VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
		VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
		VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
		VkPipelineColorBlendAttachmentState ba[MAX_COLORS];
		for (unsigned i = 0; i < color_count; i++) ba[i] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = 0xf};
		VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = color_count, .pAttachments = ba,
		                                          .flags = input_count && raster_order ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT : 0};
		VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
		VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
		                                     .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
		                                     .layout = layout, .renderPass = pass, .subpass = 0};
		result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
	}
	fflush(stderr);
	if (result != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
		printf("FAIL: the driver refused the pipeline (VkResult %d)\n", (int)result);
		return 1;
	}
	printf("PASS: the driver compiled the pipeline (%s)\n", comp ? cm.entry : fm.entry);
	return 0;
}
