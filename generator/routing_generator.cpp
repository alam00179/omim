#include "routing_generator.hpp"
#include "gen_mwm_info.hpp"
#include "borders_generator.hpp"
#include "borders_loader.hpp"

#include "../routing/osrm2feature_map.hpp"

#include "../indexer/index.hpp"
#include "../indexer/classificator_loader.hpp"
#include "../indexer/feature.hpp"
#include "../indexer/ftypes_matcher.hpp"
#include "../indexer/mercator.hpp"

#include "../coding/file_container.hpp"
#include "../coding/internal/file_data.hpp"
#include "../coding/read_write_utils.hpp"

#include "../geometry/distance_on_sphere.hpp"

#include "../base/logging.hpp"

#include "../std/fstream.hpp"

#include "../3party/osrm/osrm-backend/DataStructures/EdgeBasedNodeData.h"
#include "../3party/osrm/osrm-backend/DataStructures/QueryEdge.h"
#include "../3party/osrm/osrm-backend/DataStructures/RawRouteData.h"
#include "../routing/osrm_data_facade.hpp"
#include "../routing/osrm_router.hpp"
#include "../routing/cross_routing_context.hpp"


namespace routing
{

//Routines to get borders from tree
class GetCountriesBordersByName
{

  class GetBordersOfCountry
  {
    vector<m2::RegionD> & m_vec;
  public:
    GetBordersOfCountry(vector<m2::RegionD> & vec) : m_vec(vec) {}
    void operator()(m2::RegionD const & region)
    {
      m_vec.push_back(region);
    }

  };
  typedef vector<m2::RegionD> vec_type;
  vec_type & m_vec;
  string const & m_name;

public:
  GetCountriesBordersByName(vec_type & vec, string const & name) : m_vec(vec), m_name(name) {}
  void operator() (borders::CountryPolygons const & c)
  {
    if (c.m_name == m_name)
    {
      GetBordersOfCountry getter(m_vec);
      c.m_regions.ForEach(getter);
    }
  }
};

class GetMWMName
{
  class CheckPointInBorder
  {
    m2::PointD const & m_point;
    bool & m_inside;
  public:
    CheckPointInBorder(m2::PointD const & point, bool & inside) : m_point(point), m_inside(inside) {m_inside=false;}
    void operator()(m2::RegionD const & region)
    {
      if (region.Contains(m_point))
        m_inside=true;
    }
  };

  string  & m_name;
  m2::PointD const & m_point;
public:
  GetMWMName(string & name, m2::PointD const & point) : m_name(name), m_point(point) {}
  void operator() (borders::CountryPolygons const & c)
  {
    bool inside;
    CheckPointInBorder getter(m_point, inside);
    c.m_regions.ForEachInRect(m2::RectD(m_point, m_point), getter);
    if (inside)
      m_name = c.m_name;
  }
};

static double const EQUAL_POINT_RADIUS_M = 2.0;

void BuildRoutingIndex(string const & baseDir, string const & countryName, string const & osrmFile)
{
  classificator::Load();

  string const mwmFile = baseDir + countryName + DATA_FILE_EXTENSION;
  Index index;
  m2::RectD rect;
  if (!index.Add(mwmFile, rect))
  {
    LOG(LCRITICAL, ("MWM file not found"));
    return;
  }

  vector<m2::RegionD> regionBorders;
  routing::CrossRoutingContext crossContext;
  LOG(LINFO, ("Loading countries borders"));
  borders::CountriesContainerT m_countries;
  CHECK(borders::LoadCountriesList(baseDir, m_countries),
      ("Error loading country polygons files"));
  {
    vector<m2::RegionD> tmpRegionBorders;
    GetCountriesBordersByName getter(tmpRegionBorders, countryName);
    m_countries.ForEach(getter);
    for (m2::RegionD const& border: tmpRegionBorders)
    {
      m2::RegionD finalBorder;
      finalBorder.Data().reserve(border.Data().size());
      for (auto p = tmpRegionBorders.data()->Begin(); p<tmpRegionBorders.data()->End(); ++p)
        finalBorder.AddPoint({MercatorBounds::XToLon(p->x), MercatorBounds::YToLat(p->y)});
      regionBorders.push_back(finalBorder);
    }
  }

  gen::OsmID2FeatureID osm2ft;
  {
    FileReader reader(mwmFile + OSM2FEATURE_FILE_EXTENSION);
    ReaderSource<FileReader> src(reader);
    osm2ft.Read(src);
  }

  osrm::NodeDataVectorT nodeData;
  if (!osrm::LoadNodeDataFromFile(osrmFile + ".nodeData", nodeData))
  {
    LOG(LCRITICAL, ("Can't load node data"));
    return;
  }

  OsrmFtSegMappingBuilder mapping;

  uint32_t found = 0, all = 0, multiple = 0, equal = 0, moreThan1Seg = 0, stored = 0;

  for (size_t nodeId = 0; nodeId < nodeData.size(); ++nodeId)
  {
    auto const & data = nodeData[nodeId];

    //Check for outgoing candidates
    if (data.m_segments.size() > 0)
    {
      auto const & startSeg = data.m_segments.front();
      auto const & endSeg = data.m_segments.back();
      // Check if we have geometry for our candidate
      if (osm2ft.GetFeatureID(startSeg.wayId) || osm2ft.GetFeatureID(endSeg.wayId))
      {
        m2::PointD const pts[2] = { { startSeg.lon1, startSeg.lat1 }, { endSeg.lon2, endSeg.lat2 } };

        // check nearest to goverment borders
        for (m2::RegionD const& border: regionBorders)
        {
          bool const outStart = border.Contains(pts[0]), outEnd = border.Contains(pts[1]);
          if (outStart == true && outEnd == false)
          {
            string mwmName;
            m2::PointD mercatorPoint(MercatorBounds::LonToX(pts[1].x), MercatorBounds::LatToY(pts[1].y));
            GetMWMName getter(mwmName, mercatorPoint);
            m_countries.ForEachInRect(m2::RectD(mercatorPoint, mercatorPoint), getter);
            crossContext.addOutgoingNode(nodeId, mwmName);
          }
          else
            if (outStart == false && outEnd == true)
              crossContext.addIngoingNode(nodeId);
        }
      }
    }

    OsrmFtSegMappingBuilder::FtSegVectorT vec;

    for (auto const & seg : data.m_segments)
    {
      m2::PointD const pts[2] = { { seg.lon1, seg.lat1 }, { seg.lon2, seg.lat2 } };
      ++all;

      // now need to determine feature id and segments in it
      uint32_t const fID = osm2ft.GetFeatureID(seg.wayId);
      if (fID == 0)
      {
        LOG(LWARNING, ("No feature id for way:", seg.wayId));
        continue;
      }

      FeatureType ft;
      Index::FeaturesLoaderGuard loader(index, 0);
      loader.GetFeature(fID, ft);

      ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

      typedef pair<int, double> IndexT;
      vector<IndexT> indices[2];

      // Match input segment points on feature points.
      for (int j = 0; j < ft.GetPointsCount(); ++j)
      {
        double const lon = MercatorBounds::XToLon(ft.GetPoint(j).x);
        double const lat = MercatorBounds::YToLat(ft.GetPoint(j).y);
        for (int k = 0; k < 2; ++k)
        {
          double const dist = ms::DistanceOnEarth(pts[k].y, pts[k].x, lat, lon);
          if (dist <= EQUAL_POINT_RADIUS_M)
            indices[k].push_back(make_pair(j, dist));
        }
      }

      if (!indices[0].empty() && !indices[1].empty())
      {
        for (int k = 0; k < 2; ++k)
        {
          sort(indices[k].begin(), indices[k].end(), [] (IndexT const & r1, IndexT const & r2)
          {
            return (r1.second < r2.second);
          });
        }

        // Show warnings for multiple or equal choices.
        if (indices[0].size() != 1 && indices[1].size() != 1)
        {
          ++multiple;
          //LOG(LWARNING, ("Multiple index choices for way:", seg.wayId, indices[0], indices[1]));
        }

        {
          size_t const count = min(indices[0].size(), indices[1].size());
          size_t i = 0;
          for (; i < count; ++i)
            if (indices[0][i].first != indices[1][i].first)
              break;

          if (i == count)
          {
            ++equal;
            LOG(LWARNING, ("Equal choices for way:", seg.wayId, indices[0], indices[1]));
          }
        }

        // Find best matching for multiple choices.
        int ind1 = -1, ind2 = -1, dist = numeric_limits<int>::max();
        for (auto i1 : indices[0])
          for (auto i2 : indices[1])
          {
            int const d = abs(i1.first - i2.first);
            if (d < dist && i1.first != i2.first)
            {
              ind1 = i1.first;
              ind2 = i2.first;
              dist = d;
            }
          }

        if (ind1 != -1 && ind2 != -1)
        {
          ++found;

          // Emit segment.
          OsrmFtSegMapping::FtSeg ftSeg(fID, ind1, ind2);
          if (vec.empty() || !vec.back().Merge(ftSeg))
          {
            vec.push_back(ftSeg);
            ++stored;
          }

          continue;
        }
      }

      // Matching error.
      LOG(LWARNING, ("!!!!! Match not found:", seg.wayId));
      LOG(LWARNING, ("(Lat, Lon):", pts[0].y, pts[0].x, "; (Lat, Lon):", pts[1].y, pts[1].x));
      for (uint32_t j = 0; j < ft.GetPointsCount(); ++j)
      {
        double lon = MercatorBounds::XToLon(ft.GetPoint(j).x);
        double lat = MercatorBounds::YToLat(ft.GetPoint(j).y);
        double const dist1 = ms::DistanceOnEarth(pts[0].y, pts[0].x, lat, lon);
        double const dist2 = ms::DistanceOnEarth(pts[1].y, pts[1].x, lat, lon);
        LOG(LWARNING, ("p", j, ":", lat, lon, "Dist1:", dist1, "Dist2:", dist2));
      }
    }

    if (vec.size() > 1)
      ++moreThan1Seg;
    mapping.Append(nodeId, vec);
  }

  LOG(LINFO, ("All:", all, "Found:", found, "Not found:", all - found, "More that one segs in node:", moreThan1Seg,
              "Multiple:", multiple, "Equal:", equal));

  LOG(LINFO, ("Collect all data into one file..."));
  string const fPath = mwmFile + ROUTING_FILE_EXTENSION;

  FilesContainerW routingCont(fPath);

  {
    // Write version for routing file that is equal to correspondent mwm file.
    FilesContainerR mwmCont(mwmFile);

    FileWriter w = routingCont.GetWriter(VERSION_FILE_TAG);
    ReaderSource<ModelReaderPtr> src(mwmCont.GetReader(VERSION_FILE_TAG));
    rw::ReadAndWrite(src, w);
    w.WritePadding(4);
  }

  mapping.Save(routingCont);

  auto appendFile = [&] (string const & tag)
  {
    string const fileName = osrmFile + "." + tag;
    LOG(LINFO, ("Append file", fileName, "with tag", tag));
    routingCont.Write(fileName, tag);
  };

  appendFile(ROUTING_SHORTCUTS_FILE_TAG);
  appendFile(ROUTING_EDGEDATA_FILE_TAG);
  appendFile(ROUTING_MATRIX_FILE_TAG);
  appendFile(ROUTING_EDGEID_FILE_TAG);

  {
    // Load routing facade
    OsrmRawDataFacade<QueryEdge::EdgeData> facade;
    ReaderSource<ModelReaderPtr> edgeSource(new FileReader(osrmFile + "." + ROUTING_EDGEDATA_FILE_TAG));
    vector<char> edgeBuffer(static_cast<size_t>(edgeSource.Size()));
    edgeSource.Read(&edgeBuffer[0], edgeSource.Size());
    ReaderSource<ModelReaderPtr> edgeIdsSource(new FileReader(osrmFile + "." + ROUTING_EDGEID_FILE_TAG));
    vector<char> edgeIdsBuffer(static_cast<size_t>(edgeIdsSource.Size()));
    edgeIdsSource.Read(&edgeIdsBuffer[0], edgeIdsSource.Size());
    ReaderSource<ModelReaderPtr> shortcutsSource(new FileReader(osrmFile + "." + ROUTING_SHORTCUTS_FILE_TAG));
    vector<char> shortcutsBuffer(static_cast<size_t>(shortcutsSource.Size()));
    shortcutsSource.Read(&shortcutsBuffer[0], shortcutsSource.Size());
    ReaderSource<ModelReaderPtr> matrixSource(new FileReader(osrmFile + "." + ROUTING_MATRIX_FILE_TAG));
    vector<char> matrixBuffer(static_cast<size_t>(matrixSource.Size()));
    matrixSource.Read(&matrixBuffer[0], matrixSource.Size());
    facade.LoadRawData(edgeBuffer.data(), edgeIdsBuffer.data(), shortcutsBuffer.data(), matrixBuffer.data());

    LOG(LINFO, ("Calculating weight map between outgoing nodes"));
    crossContext.reserveAdjacencyMatrix();
    auto in = crossContext.GetIngoingIterators();
    auto out = crossContext.GetOutgoingIterators();
    MultiroutingTaskPointT sources(distance(in.first, in.second)), targets(distance(out.first, out.second));
    for (auto i = in.first; i < in.second; ++i)
    {
      OsrmRouter::GenerateRoutingTaskFromNodeId(*i, sources[distance(in.first, i)]);
    }
    for (auto i = out.first; i < out.second; ++i)
    {
      OsrmRouter::GenerateRoutingTaskFromNodeId(i->first, targets[distance(out.first, i)]);
    }

    vector<EdgeWeight> costs;
    OsrmRouter::FindWeightsMatrix(sources, targets, facade, costs);
    auto res = costs.begin();
    for (auto i = in.first; i < in.second; ++i)
      for (auto j = out.first; j < out.second; ++j)
      {
        crossContext.setAdjacencyCost(i, j, *res);
        ++res;
      }
    LOG(LINFO, ("Calculating weight map between outgoing nodes DONE"));
  }

  FileWriter w = routingCont.GetWriter(ROUTING_CROSS_CONTEXT_TAG);
  crossContext.Save(w);
  w.WritePadding(4);
  LOG(LINFO, ("Write routing info. Have", w.Pos(), "bytes"));

  routingCont.Finish();

  uint64_t sz;
  VERIFY(my::GetFileSize(fPath, sz), ());
  LOG(LINFO, ("Nodes stored:", stored, "Routing index file size:", sz));
}
}
