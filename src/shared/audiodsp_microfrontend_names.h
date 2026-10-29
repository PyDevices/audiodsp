// Every external symbol the microfrontend and kissfft define, renamed into
// audiodsp's namespace.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiodsp_microfrontend.c includes this before any of the library, and so
// does every file that calls it, so declarations, definitions and calls all
// come out as audiodsp_mf_*. Without it, a firmware that also links another
// copy of the frontend or of kissfft fails to link on the first duplicate.
//
// The list is every global symbol in the compiled translation unit.
// tests/test_cpython_audiofeatures.py fails on any symbol that escaped, so an
// update that adds a function shows up there.

#pragma once

#define FftCompute                               audiodsp_mf_FftCompute
#define FftFreeStateContents                     audiodsp_mf_FftFreeStateContents
#define FftInit                                  audiodsp_mf_FftInit
#define FftPopulateState                         audiodsp_mf_FftPopulateState
#define FftReset                                 audiodsp_mf_FftReset
#define FilterbankAccumulateChannels             audiodsp_mf_FilterbankAccumulateChannels
#define FilterbankConvertFftComplexToEnergy      audiodsp_mf_FilterbankConvertFftComplexToEnergy
#define FilterbankFillConfigWithDefaults         audiodsp_mf_FilterbankFillConfigWithDefaults
#define FilterbankFreeStateContents              audiodsp_mf_FilterbankFreeStateContents
#define FilterbankPopulateState                  audiodsp_mf_FilterbankPopulateState
#define FilterbankReset                          audiodsp_mf_FilterbankReset
#define FilterbankSqrt                           audiodsp_mf_FilterbankSqrt
#define FrontendFillConfigWithDefaults           audiodsp_mf_FrontendFillConfigWithDefaults
#define FrontendFreeStateContents                audiodsp_mf_FrontendFreeStateContents
#define FrontendPopulateState                    audiodsp_mf_FrontendPopulateState
#define FrontendProcessSamples                   audiodsp_mf_FrontendProcessSamples
#define FrontendReset                            audiodsp_mf_FrontendReset
#define LogScaleApply                            audiodsp_mf_LogScaleApply
#define LogScaleFillConfigWithDefaults           audiodsp_mf_LogScaleFillConfigWithDefaults
#define LogScalePopulateState                    audiodsp_mf_LogScalePopulateState
#define NoiseReductionApply                      audiodsp_mf_NoiseReductionApply
#define NoiseReductionFillConfigWithDefaults     audiodsp_mf_NoiseReductionFillConfigWithDefaults
#define NoiseReductionFreeStateContents          audiodsp_mf_NoiseReductionFreeStateContents
#define NoiseReductionPopulateState              audiodsp_mf_NoiseReductionPopulateState
#define NoiseReductionReset                      audiodsp_mf_NoiseReductionReset
#define PcanGainControlApply                     audiodsp_mf_PcanGainControlApply
#define PcanGainControlFillConfigWithDefaults    audiodsp_mf_PcanGainControlFillConfigWithDefaults
#define PcanGainControlFreeStateContents         audiodsp_mf_PcanGainControlFreeStateContents
#define PcanGainControlPopulateState             audiodsp_mf_PcanGainControlPopulateState
#define PcanGainLookupFunction                   audiodsp_mf_PcanGainLookupFunction
#define PcanShrink                               audiodsp_mf_PcanShrink
#define WideDynamicFunction                      audiodsp_mf_WideDynamicFunction
#define WindowFillConfigWithDefaults             audiodsp_mf_WindowFillConfigWithDefaults
#define WindowFreeStateContents                  audiodsp_mf_WindowFreeStateContents
#define WindowPopulateState                      audiodsp_mf_WindowPopulateState
#define WindowProcessSamples                     audiodsp_mf_WindowProcessSamples
#define WindowReset                              audiodsp_mf_WindowReset
#define kLogLut                                  audiodsp_mf_kLogLut
#define kiss_fft                                 audiodsp_mf_kiss_fft
#define kiss_fft_alloc                           audiodsp_mf_kiss_fft_alloc
#define kiss_fft_cleanup                         audiodsp_mf_kiss_fft_cleanup
#define kiss_fft_next_fast_size                  audiodsp_mf_kiss_fft_next_fast_size
#define kiss_fft_stride                          audiodsp_mf_kiss_fft_stride
#define kiss_fftr                                audiodsp_mf_kiss_fftr
#define kiss_fftr_alloc                          audiodsp_mf_kiss_fftr_alloc
#define kiss_fftri                               audiodsp_mf_kiss_fftri
