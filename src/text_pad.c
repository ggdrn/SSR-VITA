// vita-elf-create needs ~4 KB free between the end of the text segment and the
// 64 KB-aligned data segment. When a build lands too close, scripts/build.sh
// reconfigures with SRR_TEXT_PAD to push the data segment to the next boundary.
#if SRR_TEXT_PAD > 0
__attribute__((used)) const char srr_text_pad[SRR_TEXT_PAD] = {1};
#endif
