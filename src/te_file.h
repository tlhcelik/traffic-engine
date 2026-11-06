#ifndef TE_FILE_H
#define TE_FILE_H

#include <QByteArray>
#include <QString>

class world_model;

// te_file
// -------
// The workspace file (*.te). XML through Xerces-C (xml_doc):
//
//   <traffic_engine_workspace format="1" program_version="0.1.0">
//     <world name="..." ground="grass" start_clock="28800" geo_lat="0" geo_lon="0">
//       <description>...</description>
//     </world>
//     <nodes>   <node id=".." x=".." y=".." spawn=".." vehicles_per_hour=".." .../> </nodes>
//     <roads>   <road id=".." a=".." b=".." lanes_forward=".." ...><shape>x,y x,y</shape></road> </roads>
//     <lights>  <light id=".." node=".." algorithm=".." ...><phase green=".." roads="3 4" .../></light> </lights>
//     <signs>   <sign id=".." key=".." road=".." s=".." dir=".." value=".."/> </signs>
//     <objects> <object id=".." kind=".." road=".." s=".." .../> </objects>
//   </traffic_engine_workspace>
//
// The save is atomic (QSaveFile). load() checks the references (a road to a
// node that does not exist is removed, with a log line).

namespace te_file
{
	extern const char *const extension;   // "te"
	bool save(const world_model &model, const QString &path, QString *error = nullptr);
	bool load(world_model &model, const QString &path, QString *error = nullptr);
	QByteArray to_bytes(const world_model &model);
	bool from_bytes(world_model &model, const QByteArray &data, const QString &source, QString *error = nullptr);
}

#endif // TE_FILE_H
