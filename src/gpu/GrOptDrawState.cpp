/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrOptDrawState.h"

#include "GrDrawState.h"
#include "GrDrawTargetCaps.h"
#include "GrGpu.h"
#include "GrProcOptInfo.h"
#include "GrXferProcessor.h"

GrOptDrawState::GrOptDrawState(const GrDrawState& drawState,
                               const GrPrimitiveProcessor* primProc,
                               const GrDrawTargetCaps& caps,
                               const GrScissorState& scissorState,
                               const GrDeviceCoordTexture* dstCopy,
                               GrGpu::DrawType drawType) {
    fDrawType = drawType;

    const GrProcOptInfo& colorPOI = drawState.colorProcInfo(primProc);
    const GrProcOptInfo& coveragePOI = drawState.coverageProcInfo(primProc);

    // Create XferProcessor from DS's XPFactory
    SkAutoTUnref<GrXferProcessor> xferProcessor(
        drawState.getXPFactory()->createXferProcessor(colorPOI, coveragePOI));

    GrColor overrideColor = GrColor_ILLEGAL;
    if (colorPOI.firstEffectiveStageIndex() != 0) {
        overrideColor = colorPOI.inputColorToEffectiveStage();
    }

    GrXferProcessor::OptFlags optFlags;
    if (xferProcessor) {
        fXferProcessor.reset(xferProcessor.get());

        optFlags = xferProcessor->getOptimizations(colorPOI,
                                                   coveragePOI,
                                                   drawState.getStencil().doesWrite(),
                                                   &overrideColor,
                                                   caps);
    }

    // When path rendering the stencil settings are not always set on the draw state
    // so we must check the draw type. In cases where we will skip drawing we simply return a
    // null GrOptDrawState.
    if (!xferProcessor || (GrXferProcessor::kSkipDraw_OptFlag & optFlags)) {
        // Set the fields that don't default init and return. The lack of a render target will
        // indicate that this can be skipped.
        fFlags = 0;
        fDrawFace = GrDrawState::kInvalid_DrawFace;
        return;
    }

    fRenderTarget.reset(drawState.fRenderTarget.get());
    SkASSERT(fRenderTarget);
    fScissorState = scissorState;
    fStencilSettings = drawState.getStencil();
    fDrawFace = drawState.getDrawFace();
    // TODO move this out of optDrawState
    if (dstCopy) {
        fDstCopy = *dstCopy;
    }

    fFlags = 0;
    if (drawState.isHWAntialias()) {
        fFlags |= kHWAA_Flag;
    }
    if (drawState.isDither()) {
        fFlags |= kDither_Flag;
    }

    int firstColorStageIdx = colorPOI.firstEffectiveStageIndex();

    // TODO: Once we can handle single or four channel input into coverage stages then we can use
    // drawState's coverageProcInfo (like color above) to set this initial information.
    int firstCoverageStageIdx = 0;

    GrXferProcessor::BlendInfo blendInfo;
    fXferProcessor->getBlendInfo(&blendInfo);

    this->adjustProgramFromOptimizations(drawState, optFlags, colorPOI, coveragePOI,
                                         &firstColorStageIdx, &firstCoverageStageIdx);

    fDescInfo.fReadsDst = fXferProcessor->willReadDstColor();

    bool usesLocalCoords = false;

    // Copy Stages from DS to ODS
    for (int i = firstColorStageIdx; i < drawState.numColorStages(); ++i) {
        SkNEW_APPEND_TO_TARRAY(&fFragmentStages,
                               GrPendingFragmentStage,
                               (drawState.fColorStages[i]));
        usesLocalCoords = usesLocalCoords ||
                          drawState.fColorStages[i].processor()->usesLocalCoords();
    }

    fNumColorStages = fFragmentStages.count();
    for (int i = firstCoverageStageIdx; i < drawState.numCoverageStages(); ++i) {
        SkNEW_APPEND_TO_TARRAY(&fFragmentStages,
                               GrPendingFragmentStage,
                               (drawState.fCoverageStages[i]));
        usesLocalCoords = usesLocalCoords ||
                          drawState.fCoverageStages[i].processor()->usesLocalCoords();
    }

    // let the GP init the batch tracker
    fInitBT.fColorIgnored = SkToBool(optFlags & GrXferProcessor::kIgnoreColor_OptFlag);
    fInitBT.fOverrideColor = fInitBT.fColorIgnored ? GrColor_ILLEGAL : overrideColor;
    fInitBT.fCoverageIgnored = SkToBool(optFlags & GrXferProcessor::kIgnoreCoverage_OptFlag);
    fInitBT.fUsesLocalCoords = usesLocalCoords;
}

void GrOptDrawState::adjustProgramFromOptimizations(const GrDrawState& ds,
                                                    GrXferProcessor::OptFlags flags,
                                                    const GrProcOptInfo& colorPOI,
                                                    const GrProcOptInfo& coveragePOI,
                                                    int* firstColorStageIdx,
                                                    int* firstCoverageStageIdx) {
    fDescInfo.fReadsFragPosition = false;

    if ((flags & GrXferProcessor::kIgnoreColor_OptFlag) ||
        (flags & GrXferProcessor::kOverrideColor_OptFlag)) {
        *firstColorStageIdx = ds.numColorStages();
    } else {
        fDescInfo.fReadsFragPosition = colorPOI.readsFragPosition();
    }

    if (flags & GrXferProcessor::kIgnoreCoverage_OptFlag) {
        *firstCoverageStageIdx = ds.numCoverageStages();
    } else {
        if (coveragePOI.readsFragPosition()) {
            fDescInfo.fReadsFragPosition = true;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

bool GrOptDrawState::isEqual(const GrOptDrawState& that) const {
    if (this->getRenderTarget() != that.getRenderTarget() ||
        this->fFragmentStages.count() != that.fFragmentStages.count() ||
        this->fNumColorStages != that.fNumColorStages ||
        this->fScissorState != that.fScissorState ||
        this->fDrawType != that.fDrawType ||
        this->fFlags != that.fFlags ||
        this->fStencilSettings != that.fStencilSettings ||
        this->fDrawFace != that.fDrawFace ||
        this->fDstCopy.texture() != that.fDstCopy.texture()) {
        return false;
    }

    if (!this->getXferProcessor()->isEqual(*that.getXferProcessor())) {
        return false;
    }

    // The program desc comparison should have already assured that the stage counts match.
    SkASSERT(this->numFragmentStages() == that.numFragmentStages());
    for (int i = 0; i < this->numFragmentStages(); i++) {

        if (this->getFragmentStage(i) != that.getFragmentStage(i)) {
            return false;
        }
    }
    return true;
}

