/* Opens the GPU driver the engine renders through; see android/vk_loader.c. */
#ifndef HENGE_VK_LOADER_H
#define HENGE_VK_LOADER_H

void *henge_vk_open_adrenotools(const char *driver_dir);
void *henge_vk_symbol(void *handle, const char *name);
const char *henge_vk_loader_error(void);

#endif
