#include "hclustMatrices.h"
int countPreClusterMarkers(SEXP preClusterResults_, bool& noDuplicates)
{
	Rcpp::List preClusterResults = preClusterResults_;
	std::vector<int> markers;
	for(Rcpp::List::iterator i = preClusterResults.begin(); i != preClusterResults.end(); i++)
	{
		Rcpp::IntegerVector Rmarkers = *i;
		for(Rcpp::IntegerVector::iterator j = Rmarkers.begin(); j != Rmarkers.end(); j++)
		{
			markers.push_back(*j);
		}
	}
	int nMarkers1 = markers.size();
	std::sort(markers.begin(), markers.end());
	std::vector<int>::iterator lastUnique = std::unique(markers.begin(), markers.end());
	int nMarkers2 = std::distance(markers.begin(), lastUnique);
	noDuplicates = nMarkers1 == nMarkers2;
	return nMarkers1;
}
SEXP hclustThetaMatrix(SEXP mpcrossRF_, SEXP preClusterResults_)
{
BEGIN_RCPP
	Rcpp::List preClusterResults = preClusterResults_;
	bool noDuplicates;
	int preClusterMarkers = countPreClusterMarkers(preClusterResults_, noDuplicates);
	if(!noDuplicates)
	{
		throw std::runtime_error("Duplicate marker indices in call to hclustThetaMatrix");
	}

	Rcpp::S4 mpcrossRF = mpcrossRF_;
	Rcpp::S4 rf = mpcrossRF.slot("rf");

	Rcpp::S4 theta = rf.slot("theta");
	Rcpp::RawVector data = theta.slot("data");
	Rcpp::NumericVector levels = theta.slot("levels");
	Rcpp::CharacterVector markers = theta.slot("markers");
	if(markers.size() != preClusterMarkers)
	{
		throw std::runtime_error("Number of markers in precluster object was inconsistent with number of markers in mpcrossRF object");
	}
	int resultDimension = preClusterResults.size();
	//Allocate enough storage. This symmetric matrix stores the *LOWER* triangular part, in column-major storage. Excluding the diagonal. 
	Rcpp::NumericVector result(((resultDimension-1)*resultDimension)/2);
	for(int column = 0; column < resultDimension; column++)
	{
		Rcpp::IntegerVector columnMarkers = preClusterResults(column);
		for(int row = column + 1; row < resultDimension; row++)
		{
			Rcpp::IntegerVector rowMarkers = preClusterResults(row);
			double total = 0;
			int counter = 0;
			for(int columnMarkerCounter = 0; columnMarkerCounter < columnMarkers.size(); columnMarkerCounter++)
			{
				int marker1 = columnMarkers[columnMarkerCounter]-1;
				for(int rowMarkerCounter = 0; rowMarkerCounter < rowMarkers.size(); rowMarkerCounter++)
				{
					int marker2 = rowMarkers[rowMarkerCounter]-1;
					int column = std::max(marker1, marker2);
					int row = std::min(marker1, marker2);
					Rbyte thetaDataValue = data((column*(column+1))/2 + row);
					if(thetaDataValue != 0xFF)
					{
						total += levels(thetaDataValue);
						counter++;
					}
				}
			}
			if(counter == 0) total = 0.5;
			else total /= counter;
			result(((resultDimension-1)*resultDimension)/2 - ((resultDimension - column)*(resultDimension-column-1))/2 + row-column-1) = total;
		}
	}
	return result;
END_RCPP
}
SEXP hclustCombinedMatrix(SEXP mpcrossRF_, SEXP preClusterResults_)
{
BEGIN_RCPP
	bool noDuplicates;
	int preClusterMarkers = countPreClusterMarkers(preClusterResults_, noDuplicates);
	if(!noDuplicates)
	{
		throw std::runtime_error("Duplicate marker indices in call to hclustThetaMatrix");
	}

	Rcpp::List preClusterResults = preClusterResults_;
	Rcpp::S4 mpcrossRF = mpcrossRF_;
	Rcpp::S4 rf = mpcrossRF.slot("rf");

	Rcpp::RObject lodObject = rf.slot("lod");
	if(lodObject.isNULL())
	{
		throw std::runtime_error("Slot mpcrossRF@rf@lod cannot be NULL if clusterBy is equal to \"combined\"");
	}
	Rcpp::S4 lod = Rcpp::as<Rcpp::S4>(lodObject);
	Rcpp::S4 theta = rf.slot("theta");
	Rcpp::RawVector data = theta.slot("data");
	Rcpp::NumericVector levels = theta.slot("levels");
	Rcpp::CharacterVector markers = theta.slot("markers");
	Rcpp::NumericVector lodData = lod.slot("x");
	if(markers.size() != preClusterMarkers || lodData.size() != (preClusterMarkers*(preClusterMarkers+1))/2)
	{
		throw std::runtime_error("Number of markers in precluster object was inconsistent with number of markers in mpcrossRF object");
	}
	int resultDimension = preClusterResults.size();
	//Work out minimum difference between recombination levels
	double minDifference = 1;
	for(int i = 0; i < levels.size()-1; i++)
	{
		minDifference = std::min(minDifference, levels[i+1] - levels[i]);
	}
	double maxLod = *std::max_element(lodData.begin(), lodData.end());
	double lodMultiplier = minDifference/maxLod;
	//Allocate enough storage. This symmetric matrix stores the *LOWER* triangular part, in column-major storage. Excluding the diagonal. 
	Rcpp::NumericVector result(((resultDimension-1)*resultDimension)/2);
	for(int column = 0; column < resultDimension; column++)
	{
		Rcpp::IntegerVector columnMarkers = preClusterResults(column);
		for(int row = column + 1; row < resultDimension; row++)
		{
			Rcpp::IntegerVector rowMarkers = preClusterResults(row);
			double total = 0;
			int counter = 0;
			for(int columnMarkerCounter = 0; columnMarkerCounter < columnMarkers.size(); columnMarkerCounter++)
			{
				int marker1 = columnMarkers[columnMarkerCounter]-1;
				for(int rowMarkerCounter = 0; rowMarkerCounter < rowMarkers.size(); rowMarkerCounter++)
				{
					int marker2 = rowMarkers[rowMarkerCounter]-1;
					int column = std::max(marker1, marker2);
					int row = std::min(marker1, marker2);
					Rbyte thetaDataValue = data((column*(column+1))/2 + row);
					double currentLodDataValue = lodData((column*(column+1))/2 + row);
					if(thetaDataValue != 0xFF)
					{
						total += levels(thetaDataValue) + currentLodDataValue *lodMultiplier;
						counter++;
					}
				}
			}
			if(counter == 0) total = 0.5;
			else total /= counter;
			result(((resultDimension-1)*resultDimension)/2 - ((resultDimension - column)*(resultDimension-column-1))/2 + row-column-1) = total;
		}
	}
	return result;
END_RCPP
}
SEXP hclustLodMatrix(SEXP mpcrossRF_, SEXP preClusterResults_)
{
BEGIN_RCPP
	bool noDuplicates;
	int preClusterMarkers = countPreClusterMarkers(preClusterResults_, noDuplicates);
	if(!noDuplicates)
	{
		throw std::runtime_error("Duplicate marker indices in call to hclustThetaMatrix");
	}

	Rcpp::List preClusterResults = preClusterResults_;
	Rcpp::S4 mpcrossRF = mpcrossRF_;
	Rcpp::S4 rf = mpcrossRF.slot("rf");

	Rcpp::RObject lodObject = rf.slot("lod");
	if(lodObject.isNULL())
	{
		throw std::runtime_error("Slot mpcrossRF@rf@lod cannot be NULL if clusterBy is equal to \"combined\"");
	}
	Rcpp::S4 lod = Rcpp::as<Rcpp::S4>(lodObject);
	Rcpp::NumericVector lodData = lod.slot("x");
	if(lodData.size() != (preClusterMarkers*(preClusterMarkers+1))/2)
	{
		throw std::runtime_error("Number of markers in precluster object was inconsistent with number of markers in mpcrossRF object");
	}
	int resultDimension = preClusterResults.size();
	//Allocate enough storage. This symmetric matrix stores the *LOWER* triangular part, in column-major storage. Excluding the diagonal. 
	Rcpp::NumericVector result(((resultDimension-1)*resultDimension)/2);
	for(int column = 0; column < resultDimension; column++)
	{
		Rcpp::IntegerVector columnMarkers = preClusterResults(column);
		for(int row = column + 1; row < resultDimension; row++)
		{
			Rcpp::IntegerVector rowMarkers = preClusterResults(row);
			double total = 0;
			int counter = 0;
			for(int columnMarkerCounter = 0; columnMarkerCounter < columnMarkers.size(); columnMarkerCounter++)
			{
				int marker1 = columnMarkers[columnMarkerCounter]-1;
				for(int rowMarkerCounter = 0; rowMarkerCounter < rowMarkers.size(); rowMarkerCounter++)
				{
					int marker2 = rowMarkers[rowMarkerCounter]-1;
					int column = std::max(marker1, marker2);
					int row = std::min(marker1, marker2);
					double currentLodDataValue = lodData((column*(column+1))/2 + row);
					if(currentLodDataValue != NA_REAL && currentLodDataValue == currentLodDataValue)
					{
						total += currentLodDataValue;
						counter++;
					}
				}
			}
			if(counter == 0) total = 0.5;
			else total /= counter;
			result(((resultDimension-1)*resultDimension)/2 - ((resultDimension - column)*(resultDimension-column-1))/2 + row-column-1) = total;
		}
	}
	return result;
END_RCPP;
}
