/*
-----------------------------------------------------------------------
Copyright: 2010-2018, imec Vision Lab, University of Antwerp
           2014-2018, CWI, Amsterdam

Contact: astra@astra-toolbox.com
Website: http://www.astra-toolbox.com/

This file is part of the ASTRA Toolbox.


The ASTRA Toolbox is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

The ASTRA Toolbox is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with the ASTRA Toolbox. If not, see <http://www.gnu.org/licenses/>.

-----------------------------------------------------------------------
*/

#include <astra/CudaFilteredBackProjectionAlgorithm.h>
#include <astra/FanFlatProjectionGeometry2D.h>
#include <cstring>

#include "astra/AstraObjectManager.h"
#include "astra/CudaProjector2D.h"
#include "astra/cuda/2d/astra.h"
#include "astra/cuda/2d/fbp.h"

#include "astra/Logging.h"

using namespace std;
using namespace astra;

string CCudaFilteredBackProjectionAlgorithm::type = "FBP_CUDA";

CCudaFilteredBackProjectionAlgorithm::CCudaFilteredBackProjectionAlgorithm()
{
	m_bIsInitialized = false;
	CCudaReconstructionAlgorithm2D::_clear();
	m_pfFilter = NULL;
	m_fFilterParameter = -1.0f;
	m_fFilterD = 1.0f;
}

CCudaFilteredBackProjectionAlgorithm::~CCudaFilteredBackProjectionAlgorithm()
{
	delete[] m_pfFilter;
	m_pfFilter = NULL;
}

bool CCudaFilteredBackProjectionAlgorithm::initialize(const Config& _cfg)
{
	ASTRA_ASSERT(_cfg.self);
	ConfigStackCheck<CAlgorithm> CC("CudaFilteredBackProjectionAlgorithm", this, _cfg);

	// if already initialized, clear first
	if (m_bIsInitialized)
	{
		clear();
	}

	m_bIsInitialized = CCudaReconstructionAlgorithm2D::initialize(_cfg);
	if (!m_bIsInitialized)
		return false;


	// filter type
	XMLNode node = _cfg.self.getSingleNode("FilterType");
	if (node)
		m_eFilter = _convertStringToFilter(node.getContent().c_str());
	else
		m_eFilter = FILTER_RAMLAK;
	CC.markNodeParsed("FilterType");

	// filter
	node = _cfg.self.getSingleNode("FilterSinogramId");
	if (node)
	{
		int id = node.getContentInt();
		const CFloat32ProjectionData2D * pFilterData = dynamic_cast<CFloat32ProjectionData2D*>(CData2DManager::getSingleton().get(id));
		m_iFilterWidth = pFilterData->getGeometry()->getDetectorCount();
		int iFilterProjectionCount = pFilterData->getGeometry()->getProjectionAngleCount();

		m_pfFilter = new float[m_iFilterWidth * iFilterProjectionCount];
		memcpy(m_pfFilter, pFilterData->getDataConst(), sizeof(float) * m_iFilterWidth * iFilterProjectionCount);
	}
	else
	{
		m_iFilterWidth = 0;
		m_pfFilter = NULL;
	}
	CC.markNodeParsed("FilterSinogramId"); // TODO: Only for some types!

	// filter parameter
	node = _cfg.self.getSingleNode("FilterParameter");
	if (node)
	{
		float fParameter = node.getContentNumerical();
		m_fFilterParameter = fParameter;
	}
	else
	{
		m_fFilterParameter = -1.0f;
	}
	CC.markNodeParsed("FilterParameter"); // TODO: Only for some types!

	// D value
	node = _cfg.self.getSingleNode("FilterD");
	if (node)
	{
		float fD = node.getContentNumerical();
		m_fFilterD = fD;
	}
	else
	{
		m_fFilterD = 1.0f;
	}
	CC.markNodeParsed("FilterD"); // TODO: Only for some types!

	// Fan beam short scan mode
	if (m_pSinogram && dynamic_cast<CFanFlatProjectionGeometry2D*>(m_pSinogram->getGeometry())) {
		m_bShortScan = (int)_cfg.self.getOptionBool("ShortScan", false);
		CC.markOptionParsed("ShortScan");
	}

	initializeFromProjector();


	m_pAlgo = new astraCUDA::FBP();
	m_bAlgoInit = false;

	return check();
}

bool CCudaFilteredBackProjectionAlgorithm::initialize(CFloat32ProjectionData2D * _pSinogram, CFloat32VolumeData2D * _pReconstruction, E_FBPFILTER _eFilter, const float * _pfFilter /* = NULL */, int _iFilterWidth /* = 0 */, int _iGPUIndex /* = 0 */, float _fFilterParameter /* = -1.0f */)
{
	// if already initialized, clear first
	if (m_bIsInitialized)
	{
		clear();
	}

	// required classes
	m_pSinogram = _pSinogram;
	m_pReconstruction = _pReconstruction;
	m_iGPUIndex = _iGPUIndex;

	m_eFilter = _eFilter;
	m_iFilterWidth = _iFilterWidth;
	m_bShortScan = false;

	// success
	m_bIsInitialized = true;

	m_pAlgo = new astraCUDA::FBP();
	m_bAlgoInit = false;

	if(_pfFilter != NULL)
	{
		int iFilterElementCount = 0;

		if((_eFilter != FILTER_SINOGRAM) && (_eFilter != FILTER_RSINOGRAM))
		{
			iFilterElementCount = _iFilterWidth;
		}
		else
		{
			iFilterElementCount = m_pSinogram->getAngleCount();
		}

		m_pfFilter = new float[iFilterElementCount];
		memcpy(m_pfFilter, _pfFilter, iFilterElementCount * sizeof(float));
	}
	else
	{
		m_pfFilter = NULL;
	}

	m_fFilterParameter = _fFilterParameter;

	return check();
}


void CCudaFilteredBackProjectionAlgorithm::initCUDAAlgorithm()
{
	CCudaReconstructionAlgorithm2D::initCUDAAlgorithm();

	astraCUDA::FBP* pFBP = dynamic_cast<astraCUDA::FBP*>(m_pAlgo);

	bool ok = pFBP->setFilter(m_eFilter, m_pfFilter, m_iFilterWidth, m_fFilterD, m_fFilterParameter);
	if (!ok) {
		ASTRA_ERROR("CCudaFilteredBackProjectionAlgorithm: Failed to set filter");
		ASTRA_ASSERT(ok);
	}

	ok &= pFBP->setShortScan(m_bShortScan);
	if (!ok) {
		ASTRA_ERROR("CCudaFilteredBackProjectionAlgorithm: Failed to set short-scan mode");
	}
}


bool CCudaFilteredBackProjectionAlgorithm::check()
{
	// check pointers
	ASTRA_CONFIG_CHECK(m_pSinogram, "FBP_CUDA", "Invalid Projection Data Object.");
	ASTRA_CONFIG_CHECK(m_pReconstruction, "FBP_CUDA", "Invalid Reconstruction Data Object.");

	if((m_eFilter == FILTER_PROJECTION) || (m_eFilter == FILTER_SINOGRAM) || (m_eFilter == FILTER_RPROJECTION) || (m_eFilter == FILTER_RSINOGRAM))
	{
		ASTRA_CONFIG_CHECK(m_pfFilter, "FBP_CUDA", "Invalid filter pointer.");
	}

	// check initializations
	ASTRA_CONFIG_CHECK(m_pSinogram->isInitialized(), "FBP_CUDA", "Projection Data Object Not Initialized.");
	ASTRA_CONFIG_CHECK(m_pReconstruction->isInitialized(), "FBP_CUDA", "Reconstruction Data Object Not Initialized.");

	// check gpu index
	ASTRA_CONFIG_CHECK(m_iGPUIndex >= -1, "FBP_CUDA", "GPUIndex must be a non-negative integer or -1.");
	// check pixel supersampling
	ASTRA_CONFIG_CHECK(m_iPixelSuperSampling >= 0, "FBP_CUDA", "PixelSuperSampling must be a non-negative integer.");


	// success
	m_bIsInitialized = true;
	return true;
}

static bool stringCompareLowerCase(const char * _stringA, const char * _stringB)
{
	int iCmpReturn = 0;

#ifdef _MSC_VER
	iCmpReturn = _stricmp(_stringA, _stringB);
#else
	iCmpReturn = strcasecmp(_stringA, _stringB);
#endif

	return (iCmpReturn == 0);
}

E_FBPFILTER CCudaFilteredBackProjectionAlgorithm::_convertStringToFilter(const char * _filterType)
{
	E_FBPFILTER output = FILTER_NONE;

	if(stringCompareLowerCase(_filterType, "ram-lak"))
	{
		output = FILTER_RAMLAK;
	}
	else if(stringCompareLowerCase(_filterType, "shepp-logan"))
	{
		output = FILTER_SHEPPLOGAN;
	}
	else if(stringCompareLowerCase(_filterType, "cosine"))
	{
		output = FILTER_COSINE;
	}
	else if(stringCompareLowerCase(_filterType, "hamming"))
	{
		output = FILTER_HAMMING;
	}
	else if(stringCompareLowerCase(_filterType, "hann"))
	{
		output = FILTER_HANN;
	}
	else if(stringCompareLowerCase(_filterType, "none"))
	{
		output = FILTER_NONE;
	}
	else if(stringCompareLowerCase(_filterType, "tukey"))
	{
		output = FILTER_TUKEY;
	}
	else if(stringCompareLowerCase(_filterType, "lanczos"))
	{
		output = FILTER_LANCZOS;
	}
	else if(stringCompareLowerCase(_filterType, "triangular"))
	{
		output = FILTER_TRIANGULAR;
	}
	else if(stringCompareLowerCase(_filterType, "gaussian"))
	{
		output = FILTER_GAUSSIAN;
	}
	else if(stringCompareLowerCase(_filterType, "barlett-hann"))
	{
		output = FILTER_BARTLETTHANN;
	}
	else if(stringCompareLowerCase(_filterType, "blackman"))
	{
		output = FILTER_BLACKMAN;
	}
	else if(stringCompareLowerCase(_filterType, "nuttall"))
	{
		output = FILTER_NUTTALL;
	}
	else if(stringCompareLowerCase(_filterType, "blackman-harris"))
	{
		output = FILTER_BLACKMANHARRIS;
	}
	else if(stringCompareLowerCase(_filterType, "blackman-nuttall"))
	{
		output = FILTER_BLACKMANNUTTALL;
	}
	else if(stringCompareLowerCase(_filterType, "flat-top"))
	{
		output = FILTER_FLATTOP;
	}
	else if(stringCompareLowerCase(_filterType, "kaiser"))
	{
		output = FILTER_KAISER;
	}
	else if(stringCompareLowerCase(_filterType, "parzen"))
	{
		output = FILTER_PARZEN;
	}
	else if(stringCompareLowerCase(_filterType, "projection"))
	{
		output = FILTER_PROJECTION;
	}
	else if(stringCompareLowerCase(_filterType, "sinogram"))
	{
		output = FILTER_SINOGRAM;
	}
	else if(stringCompareLowerCase(_filterType, "rprojection"))
	{
		output = FILTER_RPROJECTION;
	}
	else if(stringCompareLowerCase(_filterType, "rsinogram"))
	{
		output = FILTER_RSINOGRAM;
	}
	else
	{
		ASTRA_ERROR("Failed to convert \"%s\" into a filter.",_filterType);
	}

	return output;
}

