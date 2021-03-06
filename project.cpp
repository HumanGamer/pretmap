#include "parseutil.h"
#include "project.h"
#include "tile.h"
#include "tileset.h"
#include "metatile.h"
#include "event.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QStandardItem>
#include <QMessageBox>
#include <QRegularExpression>

Project::Project()
{
    groupNames = new QStringList;
    map_groups = new QMap<QString, int>;
    mapNames = new QStringList;
    itemNames = new QStringList;
    flagNames = new QStringList;
    varNames = new QStringList;
    map_cache = new QMap<QString, Map*>;
    mapConstantsToMapNames = new QMap<QString, QString>;
    mapNamesToMapConstants = new QMap<QString, QString>;
    tileset_cache = new QMap<QString, Tileset*>;
}

QString Project::getProjectTitle() {
    if (!root.isNull()) {
        return root.section('/', -1);
    } else {
        return QString();
    }
}

Map* Project::loadMap(QString map_name) {
    Map *map;
    if (map_cache->contains(map_name)) {
        map = map_cache->value(map_name);
        // TODO: uncomment when undo/redo history is fully implemented for all actions.
        if (true/*map->hasUnsavedChanges()*/) {
            return map;
        }
    } else {
        map = new Map;
        map->setName(map_name);
    }

    readMapHeader(map);
    readMapAttributes(map);
    getTilesets(map);
    loadBlockdata(map);
    loadMapBorder(map);
    readMapEvents(map);
    loadMapConnections(map);
    map->commit();
    map->history.save();

    map_cache->insert(map_name, map);
    return map;
}

void Project::loadMapConnections(Map *map) {
    if (!map->isPersistedToFile) {
        return;
    }

    map->connections.clear();
    map->connection_items.clear();
    if (!map->connections_label.isNull()) {
        QString path = root + QString("/data/maps/%1/connections.inc").arg(map->name);
        QString text = readTextFile(path);
        if (!text.isNull()) {
            QList<QStringList> *commands = parseAsm(text);
            QStringList *list = getLabelValues(commands, map->connections_label);

            //// Avoid using this value. It ought to be generated instead.
            //int num_connections = list->value(0).toInt(nullptr, 0);

            QString connections_list_label = list->value(1);
            QList<QStringList> *connections = getLabelMacros(commands, connections_list_label);
            for (QStringList command : *connections) {
                QString macro = command.value(0);
                if (macro == "connection") {
                    Connection *connection = new Connection;
                    connection->direction = command.value(1);
                    connection->offset = command.value(2);
                    QString mapConstant = command.value(3);
                    if (mapConstantsToMapNames->contains(mapConstant)) {
                        connection->map_name = mapConstantsToMapNames->value(mapConstant);
                        map->connections.append(connection);
                    } else {
                        qDebug() << QString("Failed to find connected map for map constant '%1'").arg(mapConstant);
                    }
                }
            }
        }
    }
}

void Project::setNewMapConnections(Map *map) {
    map->connections.clear();
}

QList<QStringList>* Project::getLabelMacros(QList<QStringList> *list, QString label) {
    bool in_label = false;
    QList<QStringList> *new_list = new QList<QStringList>;
    for (int i = 0; i < list->length(); i++) {
        QStringList params = list->value(i);
        QString macro = params.value(0);
        if (macro == ".label") {
            if (params.value(1) == label) {
                in_label = true;
            } else if (in_label) {
                // If nothing has been read yet, assume the label
                // we're looking for is in a stack of labels.
                if (new_list->length() > 0) {
                    break;
                }
            }
        } else if (in_label) {
            new_list->append(params);
        }
    }
    return new_list;
}

// For if you don't care about filtering by macro,
// and just want all values associated with some label.
QStringList* Project::getLabelValues(QList<QStringList> *list, QString label) {
    list = getLabelMacros(list, label);
    QStringList *values = new QStringList;
    for (int i = 0; i < list->length(); i++) {
        QStringList params = list->value(i);
        QString macro = params.value(0);
        // Ignore .align
        if (macro == ".align") {
            continue;
        }
        for (int j = 1; j < params.length(); j++) {
            values->append(params.value(j));
        }
    }
    return values;
}

void Project::readMapHeader(Map* map) {
    if (!map->isPersistedToFile) {
        return;
    }

    QString label = map->name;
    ParseUtil *parser = new ParseUtil;

    QString header_text = readTextFile(root + "/data/maps/" + label + "/header.inc");
    if (header_text.isNull()) {
        return;
    }
    QStringList *header = getLabelValues(parser->parseAsm(header_text), label);
    map->attributes_label = header->value(0);
    map->events_label = header->value(1);
    map->scripts_label = header->value(2);
    map->connections_label = header->value(3);
    map->song = header->value(4);
    map->index = header->value(5);
    map->location = header->value(6);
    map->visibility = header->value(7);
    map->weather = header->value(8);
    map->type = header->value(9);
    map->unknown = header->value(10);
    map->show_location = header->value(11);
    map->battle_scene = header->value(12);
}

void Project::setNewMapHeader(Map* map, int mapIndex) {
    map->attributes_label = QString("%1_MapAttributes").arg(map->name);
    map->events_label = QString("%1_MapEvents").arg(map->name);;
    map->scripts_label = QString("%1_MapScripts").arg(map->name);;
    map->connections_label = "0x0";
    map->song = "BGM_DAN02";
    map->index = QString("%1").arg(mapIndex);
    map->location = "0";
    map->visibility = "0";
    map->weather = "2";
    map->type = "1";
    map->unknown = "0";
    map->show_location = "1";
    map->battle_scene = "0";
}

void Project::saveMapHeader(Map *map) {
    QString label = map->name;
    QString header_path = root + "/data/maps/" + label + "/header.inc";
    QString text = "";
    text += QString("%1::\n").arg(label);
    text += QString("\t.4byte %1\n").arg(map->attributes_label);
    text += QString("\t.4byte %1\n").arg(map->events_label);
    text += QString("\t.4byte %1\n").arg(map->scripts_label);

    if (map->connections.length() == 0) {
        map->connections_label = "0x0";
    } else {
        map->connections_label = QString("%1_MapConnections").arg(map->name);
    }
    text += QString("\t.4byte %1\n").arg(map->connections_label);

    text += QString("\t.2byte %1\n").arg(map->song);
    text += QString("\t.2byte %1\n").arg(map->index);
    text += QString("\t.byte %1\n").arg(map->location);
    text += QString("\t.byte %1\n").arg(map->visibility);
    text += QString("\t.byte %1\n").arg(map->weather);
    text += QString("\t.byte %1\n").arg(map->type);
    text += QString("\t.2byte %1\n").arg(map->unknown);
    text += QString("\t.byte %1\n").arg(map->show_location);
    text += QString("\t.byte %1\n").arg(map->battle_scene);
    saveTextFile(header_path, text);
}

void Project::saveMapConnections(Map *map) {
    QString path = root + "/data/maps/" + map->name + "/connections.inc";
    if (map->connections.length() > 0) {
        QString text = "";
        QString connectionsListLabel = QString("%1_MapConnectionsList").arg(map->name);
        int numValidConnections = 0;
        text += QString("%1::\n").arg(connectionsListLabel);
        for (Connection* connection : map->connections) {
            if (mapNamesToMapConstants->contains(connection->map_name)) {
                text += QString("\tconnection %1, %2, %3\n")
                        .arg(connection->direction)
                        .arg(connection->offset)
                        .arg(mapNamesToMapConstants->value(connection->map_name));
                numValidConnections++;
            } else {
                qDebug() << QString("Failed to write map connection. %1 not a valid map name").arg(connection->map_name);
            }
        }
        text += QString("\n");
        text += QString("%1::\n").arg(map->connections_label);
        text += QString("\t.4byte %1\n").arg(numValidConnections);
        text += QString("\t.4byte %1\n").arg(connectionsListLabel);
        saveTextFile(path, text);
    } else {
        deleteFile(path);
    }

    updateMapsWithConnections(map);
}

void Project::updateMapsWithConnections(Map *map) {
    if (map->connections.length() > 0) {
        if (!mapsWithConnections.contains(map->name)) {
            mapsWithConnections.append(map->name);
        }
    } else {
        if (mapsWithConnections.contains(map->name)) {
            mapsWithConnections.removeOne(map->name);
        }
    }
}

void Project::readMapAttributesTable() {
    int curMapIndex = 1;
    QString attributesText = readTextFile(getMapAttributesTableFilepath());
    QList<QStringList>* values = parseAsm(attributesText);
    bool inAttributePointers = false;
    for (int i = 0; i < values->length(); i++) {
        QStringList params = values->value(i);
        QString macro = params.value(0);
        if (macro == ".label") {
            if (inAttributePointers) {
                break;
            }
            if (params.value(1) == "gMapAttributes") {
                inAttributePointers = true;
            }
        } else if (macro == ".4byte" && inAttributePointers) {
            QString mapName = params.value(1);
            if (!mapName.contains("UnknownMapAttributes")) {
                // Strip off "_MapAttributes" from the label if it's a real map label.
                mapName = mapName.remove(mapName.length() - 14, 14);
            }
            mapAttributesTable.insert(curMapIndex, mapName);
            curMapIndex++;
        }
    }

    // Deep copy
    mapAttributesTableMaster = mapAttributesTable;
    mapAttributesTableMaster.detach();
}

void Project::saveMapAttributesTable() {
    QString text = "";
    text += QString("\t.align 2\n");
    text += QString("gMapAttributes::\n");
    for (int i = 0; i < mapAttributesTableMaster.count(); i++) {
        int mapIndex = i + 1;
        QString mapName = mapAttributesTableMaster.value(mapIndex);
        if (!mapName.contains("UnknownMapAttributes")) {
            text += QString("\t.4byte %1_MapAttributes\n").arg(mapName);
        } else {
            text += QString("\t.4byte %1\n").arg(mapName);
        }
    }
    saveTextFile(getMapAttributesTableFilepath(), text);
}

QString Project::getMapAttributesTableFilepath() {
    return QString("%1/data/maps/attributes_table.inc").arg(root);
}

void Project::readMapAttributes(Map* map) {
    if (!map->isPersistedToFile) {
        return;
    }

    ParseUtil *parser = new ParseUtil;

    QString assets_text = readTextFile(getMapAssetsFilepath());
    if (assets_text.isNull()) {
        return;
    }
    QStringList *attributes = getLabelValues(parser->parseAsm(assets_text), map->attributes_label);
    map->width = attributes->value(0);
    map->height = attributes->value(1);
    map->border_label = attributes->value(2);
    map->blockdata_label = attributes->value(3);
    map->tileset_primary_label = attributes->value(4);
    map->tileset_secondary_label = attributes->value(5);
}

void Project::readAllMapAttributes() {
    mapAttributes.clear();

    ParseUtil *parser = new ParseUtil;
    QString assets_text = readTextFile(getMapAssetsFilepath());
    if (assets_text.isNull()) {
        return;
    }

    QList<QStringList> *commands = parser->parseAsm(assets_text);

    // Assume the _assets.inc file is grouped consistently in the order of:
    // 1. <map_name>_MapBorder
    // 2. <map_name>_MapBlockdata
    // 3. <map_name>_MapAttributes
    int i = 0;
    while (i < commands->length()) {
        // Read MapBorder assets.
        QStringList borderParams = commands->value(i++);
        bool isUnknownMapBorder = borderParams.value(1).startsWith("UnknownMapBorder_");
        if (borderParams.value(0) != ".label" || (!borderParams.value(1).endsWith("_MapBorder") && !isUnknownMapBorder)) {
            qDebug() << QString("Expected MapBorder label, but found %1").arg(borderParams.value(1));
            continue;
        }
        QString borderLabel = borderParams.value(1);
        QString mapName;
        if (!isUnknownMapBorder) {
            mapName = borderLabel.remove(borderLabel.length() - 10, 10);
        } else {
            // Unknown map name has to match the MapAttributes label.
            mapName = borderLabel.replace("Border", "Attributes");
        }

        mapAttributes[mapName].insert("border_label", borderParams.value(1));
        borderParams = commands->value(i++);
        mapAttributes[mapName].insert("border_filepath", borderParams.value(1).replace("\"", ""));

        // Read MapBlockData assets.
        QStringList blockDataParams = commands->value(i++);
        bool isUnknownMapBlockdata = blockDataParams.value(1).startsWith("UnknownMapBlockdata_");
        if (blockDataParams.value(0) != ".label" || (!blockDataParams.value(1).endsWith("_MapBlockdata") && !isUnknownMapBlockdata)) {
            qDebug() << QString("Expected MapBlockdata label, but found %1").arg(blockDataParams.value(1));
            continue;
        }
        QString blockDataLabel = blockDataParams.value(1);
        mapAttributes[mapName].insert("blockdata_label", blockDataLabel);
        blockDataParams = commands->value(i++);
        mapAttributes[mapName].insert("blockdata_filepath", blockDataParams.value(1).replace("\"", ""));

        // Read MapAttributes assets.
        i++; // skip .align
        // Maps can share MapAttributes, so  gather a list of them.
        QStringList attributeMapLabels;
        QStringList attributesParams;
        QStringList* sharedAttrMaps = new QStringList;
        while (i < commands->length()) {
            attributesParams = commands->value(i);
            if (attributesParams.value(0) != ".label") {
                break;
            }
            QString attrLabel = attributesParams.value(1);
            attributeMapLabels.append(attrLabel);
            sharedAttrMaps->append(attrLabel);
            i++;
        }

        // Apply the map attributes to each of the shared maps.
        QString attrWidth = commands->value(i++).value(1);
        QString attrHeight = commands->value(i++).value(1);
        QString attrBorderLabel = commands->value(i++).value(1);
        QString attrBlockdataLabel = commands->value(i++).value(1);
        QString attrTilesetPrimary = commands->value(i++).value(1);
        QString attrTilesetSecondary = commands->value(i++).value(1);
        for (QString attributeMapLabel: attributeMapLabels) {
            QString altMapName = attributeMapLabel;
            if (!altMapName.startsWith("UnknownMapAttributes_")) {
                altMapName.remove(altMapName.length() - 14, 14);
            }
            if (!mapAttributes.contains(altMapName)) {
                mapAttributes.insert(altMapName, QMap<QString, QString>());
            }
            mapAttributes[altMapName].insert("attributes_label", attributeMapLabel);
            mapAttributes[altMapName].insert("width", attrWidth);
            mapAttributes[altMapName].insert("height", attrHeight);
            mapAttributes[altMapName].insert("border_label", attrBorderLabel);
            mapAttributes[altMapName].insert("blockdata_label", attrBlockdataLabel);
            mapAttributes[altMapName].insert("tileset_primary", attrTilesetPrimary);
            mapAttributes[altMapName].insert("tileset_secondary", attrTilesetSecondary);

            if (sharedAttrMaps->length() > 1) {
                mapAttributes[altMapName].insert("shared_attr_maps", sharedAttrMaps->join(":"));
            }
        }
    }

    // Deep copy
    mapAttributesMaster = mapAttributes;
    mapAttributesMaster.detach();
}

void Project::saveAllMapAttributes() {
    QString text = "";
    for (int i = 0; i < mapAttributesTableMaster.count(); i++) {
        int mapIndex = i + 1;
        QString mapName = mapAttributesTableMaster.value(mapIndex);
        QMap<QString, QString> attrs = mapAttributesMaster.value(mapName);

        // Find the map attributes object that contains the border data.
        QMap<QString, QString> attrsWithBorder;
        if (attrs.contains("border_filepath")) {
            attrsWithBorder = attrs;
        } else {
            QStringList labels = attrs.value("shared_attr_maps").split(":");
            for (QString label : labels) {
                label.remove(label.length() - 14, 14);
                if (mapAttributesMaster.contains(label) && mapAttributesMaster.value(label).contains("border_filepath")) {
                    attrsWithBorder = mapAttributesMaster.value(label);
                    break;
                }
            }
        }
        if (!attrsWithBorder.isEmpty()) {
            text += QString("%1::\n").arg(attrsWithBorder.value("border_label"));
            text += QString("\t.incbin \"%1\"\n").arg(attrsWithBorder.value("border_filepath"));
            text += QString("\n");
        }

        // Find the map attributes object that contains the blockdata.
        QMap<QString, QString> attrsWithBlockdata;
        if (attrs.contains("blockdata_filepath")) {
            attrsWithBlockdata = attrs;
        } else {
            QStringList labels = attrs["shared_attr_maps"].split(":");
            for (QString label : labels) {
                label.remove(label.length() - 14, 14);
                if (mapAttributesMaster.contains(label) && mapAttributesMaster.value(label).contains("blockdata_filepath")) {
                    attrsWithBlockdata = mapAttributesMaster.value(label);
                    break;
                }
            }
        }
        if (!attrsWithBlockdata.isEmpty()) {
            text += QString("%1::\n").arg(attrsWithBlockdata.value("blockdata_label"));
            text += QString("\t.incbin \"%1\"\n").arg(attrsWithBorder.value("blockdata_filepath"));
            text += QString("\n");
        }

        text += QString("\t.align 2\n");
        if (attrs.contains("shared_attr_maps")) {
            QStringList labels = attrs.value("shared_attr_maps").split(":");
            for (QString label : labels) {
                text += QString("%1::\n").arg(label);
            }
        } else {
            text += QString("%1::\n").arg(attrs.value("attributes_label"));
        }
        text += QString("\t.4byte %1\n").arg(attrs.value("width"));
        text += QString("\t.4byte %1\n").arg(attrs.value("height"));
        text += QString("\t.4byte %1\n").arg(attrs.value("border_label"));
        text += QString("\t.4byte %1\n").arg(attrs.value("blockdata_label"));
        text += QString("\t.4byte %1\n").arg(attrs.value("tileset_primary"));
        text += QString("\t.4byte %1\n").arg(attrs.value("tileset_secondary"));
        text += QString("\n");
    }

    saveTextFile(getMapAssetsFilepath(), text);
}

QString Project::getMapAssetsFilepath() {
    return root + "/data/maps/_assets.inc";
}

void Project::setNewMapAttributes(Map* map) {
    map->width = "20";
    map->height = "20";
    map->border_label = QString("%1_MapBorder").arg(map->name);
    map->blockdata_label = QString("%1_MapBlockdata").arg(map->name);
    map->tileset_primary_label = "gTileset_General";
    map->tileset_secondary_label = "gTileset_Petalburg";

    // Insert new entry into the global map attributes.
    QMap<QString, QString> attrs;
    attrs.insert("border_label", QString("%1_MapBorder").arg(map->name));
    attrs.insert("border_filepath", QString("data/maps/%1/border.bin").arg(map->name));
    attrs.insert("blockdata_label", QString("%1_MapBlockdata").arg(map->name));
    attrs.insert("blockdata_filepath", QString("data/maps/%1/map.bin").arg(map->name));
    attrs.insert("attributes_label", QString("%1_MapAttributes").arg(map->name));
    attrs.insert("width", map->width);
    attrs.insert("height", map->height);
    attrs.insert("tileset_primary", map->tileset_primary_label);
    attrs.insert("tileset_secondary", map->tileset_secondary_label);
    mapAttributes.insert(map->name, attrs);
}

void Project::saveMapGroupsTable() {
    QString text = "";
    int groupNum = 0;
    for (QStringList mapNames : groupedMapNames) {
        text += QString("\t.align 2\n");
        text += QString("gMapGroup%1::\n").arg(groupNum);
        for (QString mapName : mapNames) {
            text += QString("\t.4byte %1\n").arg(mapName);
        }
        text += QString("\n");
        groupNum++;
    }

    text += QString("\t.align 2\n");
    text += QString("gMapGroups::\n");
    for (int i = 0; i < groupNum; i++) {
        text += QString("\t.4byte gMapGroup%1\n").arg(i);
    }

    saveTextFile(root + "/data/maps/_groups.inc", text);
}

void Project::saveMapConstantsHeader() {
    QString text = QString("#ifndef GUARD_CONSTANTS_MAPS_H\n");
    text += QString("#define GUARD_CONSTANTS_MAPS_H\n");
    text += QString("\n");

    int groupNum = 0;
    for (QStringList mapNames : groupedMapNames) {
        text += QString("// Map Group %1\n").arg(groupNum);
        int maxLength = 0;
        for (QString mapName : mapNames) {
            QString mapConstantName = mapNamesToMapConstants->value(mapName);
            if (mapConstantName.length() > maxLength)
                maxLength = mapConstantName.length();
        }
        int groupIndex = 0;
        for (QString mapName : mapNames) {
            QString mapConstantName = mapNamesToMapConstants->value(mapName);
            text += QString("#define %1%2(%3 | (%4 << 8))\n")
                    .arg(mapConstantName)
                    .arg(QString(" ").repeated(maxLength - mapConstantName.length() + 1))
                    .arg(groupIndex)
                    .arg(groupNum);
            groupIndex++;
        }
        text += QString("\n");
        groupNum++;
    }

    text += QString("\n");
    text += QString("#define MAP_NONE (0x7F | (0x7F << 8))\n");
    text += QString("#define MAP_UNDEFINED (0xFF | (0xFF << 8))\n\n\n");
    text += QString("#define MAP_GROUP(map) (MAP_##map >> 8)\n");
    text += QString("#define MAP_NUM(map) (MAP_##map & 0xFF)\n\n");
    text += QString("#endif  // GUARD_CONSTANTS_MAPS_H\n");
    saveTextFile(root + "/include/constants/maps.h", text);
}

void Project::getTilesets(Map* map) {
    map->tileset_primary = getTileset(map->tileset_primary_label);
    map->tileset_secondary = getTileset(map->tileset_secondary_label);
}

Tileset* Project::loadTileset(QString label) {
    ParseUtil *parser = new ParseUtil;

    QString headers_text = readTextFile(root + "/data/tilesets/headers.inc");
    QStringList *values = getLabelValues(parser->parseAsm(headers_text), label);
    Tileset *tileset = new Tileset;
    tileset->name = label;
    tileset->is_compressed = values->value(0);
    tileset->is_secondary = values->value(1);
    tileset->padding = values->value(2);
    tileset->tiles_label = values->value(3);
    tileset->palettes_label = values->value(4);
    tileset->metatiles_label = values->value(5);
    tileset->metatile_attrs_label = values->value(6);
    tileset->callback_label = values->value(7);

    loadTilesetAssets(tileset);

    tileset_cache->insert(label, tileset);
    return tileset;
}

QString Project::getBlockdataPath(Map* map) {
    QString text = readTextFile(getMapAssetsFilepath());
    QStringList *values = getLabelValues(parseAsm(text), map->blockdata_label);
    QString path;
    if (!values->isEmpty()) {
        path = root + "/" + values->value(0).section('"', 1, 1);
    } else {
        path = root + "/data/maps/" + map->name + "/map.bin";
    }
    return path;
}

QString Project::getMapBorderPath(Map *map) {
    QString text = readTextFile(getMapAssetsFilepath());
    QStringList *values = getLabelValues(parseAsm(text), map->border_label);
    QString path;
    if (!values->isEmpty()) {
        path = root + "/" + values->value(0).section('"', 1, 1);
    } else {
        path = root + "/data/maps/" + map->name + "/border.bin";
    }
    return path;
}

void Project::loadBlockdata(Map* map) {
    if (!map->isPersistedToFile) {
        return;
    }

    QString path = getBlockdataPath(map);
    map->blockdata = readBlockdata(path);
}

void Project::setNewMapBlockdata(Map* map) {
    Blockdata *blockdata = new Blockdata;
    for (int i = 0; i < map->getWidth() * map->getHeight(); i++) {
        blockdata->addBlock(qint16(0x3001));
    }
    map->blockdata = blockdata;
}

void Project::loadMapBorder(Map *map) {
    if (!map->isPersistedToFile) {
        return;
    }

    QString path = getMapBorderPath(map);
    map->border = readBlockdata(path);
}

void Project::setNewMapBorder(Map *map) {
    Blockdata *blockdata = new Blockdata;
    blockdata->addBlock(qint16(0x01D4));
    blockdata->addBlock(qint16(0x01D5));
    blockdata->addBlock(qint16(0x01DC));
    blockdata->addBlock(qint16(0x01DD));
    map->border = blockdata;
}

void Project::saveMapBorder(Map *map) {
    QString path = getMapBorderPath(map);
    writeBlockdata(path, map->border);
}

void Project::saveBlockdata(Map* map) {
    QString path = getBlockdataPath(map);
    writeBlockdata(path, map->blockdata);
    map->history.save();
}

void Project::writeBlockdata(QString path, Blockdata *blockdata) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        QByteArray data = blockdata->serialize();
        file.write(data);
    }
}

void Project::saveAllMaps() {
    QList<QString> keys = map_cache->keys();
    for (int i = 0; i < keys.length(); i++) {
        QString key = keys.value(i);
        Map* map = map_cache->value(key);
        saveMap(map);
    }
}

void Project::saveMap(Map *map) {
    // Create/Modify a few collateral files for brand new maps.
    if (!map->isPersistedToFile) {
        QString newMapDataDir = QString(root + "/data/maps/%1").arg(map->name);
        if (!QDir::root().mkdir(newMapDataDir)) {
            qDebug() << "Error: failed to create directory for new map. " << newMapDataDir;
        }

        // TODO: In the future, these files needs more structure to allow for proper parsing/saving.
        // Create file data/scripts/maps/<map_name>.inc
        QString text = QString("%1_MapScripts::\n\t.byte 0\n").arg(map->name);
        saveTextFile(root + "/data/scripts/maps/" + map->name + ".inc", text);

        // Create file data/text/maps/<map_name>.inc
        saveTextFile(root + "/data/text/maps/" + map->name + ".inc", "\n");

        // Simply append to data/event_scripts.s.
        text = QString("\n\t.include \"data/scripts/maps/%1.inc\"\n").arg(map->name);
        text += QString("\t.include \"data/text/maps/%1.inc\"\n").arg(map->name);
        appendTextFile(root + "/data/event_scripts.s", text);

        // Simply append to data/map_events.s.
        text = QString("\n\t.include \"data/maps/events/%1.inc\"\n").arg(map->name);
        appendTextFile(root + "/data/map_events.s", text);

        // Simply append to data/maps/headers.inc.
        text = QString("\t.include \"data/maps/%1/header.inc\"\n").arg(map->name);
        appendTextFile(root + "/data/maps/headers.inc", text);
    }

    saveMapBorder(map);
    saveMapHeader(map);
    saveMapConnections(map);
    saveBlockdata(map);
    saveMapEvents(map);

    // Update global data structures with current map data.
    updateMapAttributes(map);

    map->isPersistedToFile = true;
}

void Project::updateMapAttributes(Map* map) {
    if (!mapAttributesTableMaster.contains(map->index.toInt())) {
        mapAttributesTableMaster.insert(map->index.toInt(), map->name);
    }

    // Deep copy
    QMap<QString, QString> attrs = mapAttributes.value(map->name);
    attrs.detach();
    mapAttributesMaster.insert(map->name, attrs);
}

void Project::saveAllDataStructures() {
    saveMapAttributesTable();
    saveAllMapAttributes();
    saveMapGroupsTable();
    saveMapConstantsHeader();
    saveMapsWithConnections();
}

void Project::loadTilesetAssets(Tileset* tileset) {
    ParseUtil* parser = new ParseUtil;
    QString category = (tileset->is_secondary == "TRUE") ? "secondary" : "primary";
    if (tileset->name.isNull()) {
        return;
    }
    QString dir_path = root + "/data/tilesets/" + category + "/" + tileset->name.replace("gTileset_", "").toLower();

    QString graphics_text = readTextFile(root + "/data/tilesets/graphics.inc");
    QList<QStringList> *graphics = parser->parseAsm(graphics_text);
    QStringList *tiles_values = getLabelValues(graphics, tileset->tiles_label);
    QStringList *palettes_values = getLabelValues(graphics, tileset->palettes_label);

    QString tiles_path;
    if (!tiles_values->isEmpty()) {
        tiles_path = root + "/" + tiles_values->value(0).section('"', 1, 1);
    } else {
        tiles_path = dir_path + "/tiles.4bpp";
        if (tileset->is_compressed == "TRUE") {
            tiles_path += ".lz";
        }
    }

    QStringList *palette_paths = new QStringList;
    if (!palettes_values->isEmpty()) {
        for (int i = 0; i < palettes_values->length(); i++) {
            QString value = palettes_values->value(i);
            palette_paths->append(root + "/" + value.section('"', 1, 1));
        }
    } else {
        QString palettes_dir_path = dir_path + "/palettes";
        for (int i = 0; i < 16; i++) {
            palette_paths->append(palettes_dir_path + "/" + QString("%1").arg(i, 2, 10, QLatin1Char('0')) + ".gbapal");
        }
    }

    QString metatiles_path;
    QString metatile_attrs_path;
    QString metatiles_text = readTextFile(root + "/data/tilesets/metatiles.inc");
    QList<QStringList> *metatiles_macros = parser->parseAsm(metatiles_text);
    QStringList *metatiles_values = getLabelValues(metatiles_macros, tileset->metatiles_label);
    if (!metatiles_values->isEmpty()) {
        metatiles_path = root + "/" + metatiles_values->value(0).section('"', 1, 1);
    } else {
        metatiles_path = dir_path + "/metatiles.bin";
    }
    QStringList *metatile_attrs_values = getLabelValues(metatiles_macros, tileset->metatile_attrs_label);
    if (!metatile_attrs_values->isEmpty()) {
        metatile_attrs_path = root + "/" + metatile_attrs_values->value(0).section('"', 1, 1);
    } else {
        metatile_attrs_path = dir_path + "/metatile_attributes.bin";
    }

    // tiles
    tiles_path = fixGraphicPath(tiles_path);
    QImage *image = new QImage(tiles_path);
    //image->setColor(0, qRgb(0xff, 0, 0)); // debug

    QList<QImage> *tiles = new QList<QImage>;
    int w = 8;
    int h = 8;
    for (int y = 0; y < image->height(); y += h)
    for (int x = 0; x < image->width(); x += w) {
        QImage tile = image->copy(x, y, w, h);
        tiles->append(tile);
    }
    tileset->tiles = tiles;

    // metatiles
    //qDebug() << metatiles_path;
    QFile metatiles_file(metatiles_path);
    if (metatiles_file.open(QIODevice::ReadOnly)) {
        QByteArray data = metatiles_file.readAll();
        int num_metatiles = data.length() / 16;
        int num_layers = 2;
        QList<Metatile*> *metatiles = new QList<Metatile*>;
        for (int i = 0; i < num_metatiles; i++) {
            Metatile *metatile = new Metatile;
            int index = i * (2 * 4 * num_layers);
            for (int j = 0; j < 4 * num_layers; j++) {
                uint16_t word = data[index++] & 0xff;
                word += (data[index++] & 0xff) << 8;
                Tile tile;
                tile.tile = word & 0x3ff;
                tile.xflip = (word >> 10) & 1;
                tile.yflip = (word >> 11) & 1;
                tile.palette = (word >> 12) & 0xf;
                metatile->tiles->append(tile);
            }
            metatiles->append(metatile);
        }
        tileset->metatiles = metatiles;
    } else {
        tileset->metatiles = new QList<Metatile*>;
        qDebug() << QString("Could not open '%1'").arg(metatiles_path);
    }

    QFile attrs_file(metatile_attrs_path);
    //qDebug() << metatile_attrs_path;
    if (attrs_file.open(QIODevice::ReadOnly)) {
        QByteArray data = attrs_file.readAll();
        int num_metatiles = data.length() / 2;
        for (int i = 0; i < num_metatiles; i++) {
            uint16_t word = data[i*2] & 0xff;
            word += (data[i*2 + 1] & 0xff) << 8;
            tileset->metatiles->value(i)->attr = word;
        }
    } else {
        qDebug() << QString("Could not open '%1'").arg(metatile_attrs_path);
    }

    // palettes
    QList<QList<QRgb>> *palettes = new QList<QList<QRgb>>;
    for (int i = 0; i < palette_paths->length(); i++) {
        QString path = palette_paths->value(i);
        // the palettes are not compressed. this should never happen. it's only a precaution.
        path = path.replace(QRegExp("\\.lz$"), "");
        // TODO default to .pal (JASC-PAL)
        // just use .gbapal for now
        QFile file(path);
        QList<QRgb> palette;
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            for (int j = 0; j < 16; j++) {
                uint16_t word = data[j*2] & 0xff;
                word += (data[j*2 + 1] & 0xff) << 8;
                int red = word & 0x1f;
                int green = (word >> 5) & 0x1f;
                int blue = (word >> 10) & 0x1f;
                QRgb color = qRgb(red * 8, green * 8, blue * 8);
                palette.prepend(color);
            }
        } else {
            for (int j = 0; j < 16; j++) {
                palette.append(qRgb(j * 16, j * 16, j * 16));
            }
            qDebug() << QString("Could not open '%1'").arg(path);
        }
        //qDebug() << path;
        palettes->append(palette);
    }
    tileset->palettes = palettes;
}

Blockdata* Project::readBlockdata(QString path) {
    Blockdata *blockdata = new Blockdata;
    //qDebug() << path;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray data = file.readAll();
        for (int i = 0; (i + 1) < data.length(); i += 2) {
            uint16_t word = (data[i] & 0xff) + ((data[i + 1] & 0xff) << 8);
            blockdata->addBlock(word);
        }
    }
    return blockdata;
}

Map* Project::getMap(QString map_name) {
    if (map_cache->contains(map_name)) {
        return map_cache->value(map_name);
    } else {
        Map *map = loadMap(map_name);
        return map;
    }
}

Tileset* Project::getTileset(QString label) {
    if (tileset_cache->contains(label)) {
        return tileset_cache->value(label);
    } else {
        Tileset *tileset = loadTileset(label);
        return tileset;
    }
}

QString Project::readTextFile(QString path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        //QMessageBox::information(0, "Error", QString("Could not open '%1': ").arg(path) + file.errorString());
        qDebug() << QString("Could not open '%1': ").arg(path) + file.errorString();
        return QString();
    }
    QTextStream in(&file);
    QString text = "";
    while (!in.atEnd()) {
        text += in.readLine() + "\n";
    }
    return text;
}

void Project::saveTextFile(QString path, QString text) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(text.toUtf8());
    } else {
        qDebug() << QString("Could not open '%1' for writing: ").arg(path) + file.errorString();
    }
}

void Project::appendTextFile(QString path, QString text) {
    QFile file(path);
    if (file.open(QIODevice::Append)) {
        file.write(text.toUtf8());
    } else {
        qDebug() << QString("Could not open '%1' for appending: ").arg(path) + file.errorString();
    }
}

void Project::deleteFile(QString path) {
    QFile file(path);
    if (file.exists() && !file.remove()) {
        qDebug() << QString("Could not delete file '%1': ").arg(path) + file.errorString();
    }
}

void Project::readMapGroups() {
    QString text = readTextFile(root + "/data/maps/_groups.inc");
    if (text.isNull()) {
        return;
    }
    ParseUtil *parser = new ParseUtil;
    QList<QStringList> *commands = parser->parseAsm(text);

    bool in_group_pointers = false;
    QStringList *groups = new QStringList;
    for (int i = 0; i < commands->length(); i++) {
        QStringList params = commands->value(i);
        QString macro = params.value(0);
        if (macro == ".label") {
            if (in_group_pointers) {
                break;
            }
            if (params.value(1) == "gMapGroups") {
                in_group_pointers = true;
            }
        } else if (macro == ".4byte") {
            if (in_group_pointers) {
                for (int j = 1; j < params.length(); j++) {
                    groups->append(params.value(j));
                }
            }
        }
    }

    QList<QStringList> groupedMaps;
    for (int i = 0; i < groups->length(); i++) {
        groupedMaps.append(QStringList());
    }

    QStringList *maps = new QStringList;
    int group = -1;
    for (int i = 0; i < commands->length(); i++) {
        QStringList params = commands->value(i);
        QString macro = params.value(0);
        if (macro == ".label") {
            group = groups->indexOf(params.value(1));
        } else if (macro == ".4byte") {
            if (group != -1) {
                for (int j = 1; j < params.length(); j++) {
                    QString mapName = params.value(j);
                    groupedMaps[group].append(mapName);
                    maps->append(mapName);
                    map_groups->insert(mapName, group);

                    // Build the mapping and reverse mapping between map constants and map names.
                    QString mapConstant = Map::mapConstantFromName(mapName);
                    mapConstantsToMapNames->insert(mapConstant, mapName);
                    mapNamesToMapConstants->insert(mapName, mapConstant);
                }
            }
        }
    }

    groupNames = groups;
    groupedMapNames = groupedMaps;
    mapNames = maps;
}

Map* Project::addNewMapToGroup(QString mapName, int groupNum) {
    int mapIndex = mapAttributesTable.count() + 1;
    mapAttributesTable.insert(mapIndex, mapName);

    // Setup new map in memory, but don't write to file until map is actually saved later.
    mapNames->append(mapName);
    map_groups->insert(mapName, groupNum);
    groupedMapNames[groupNum].append(mapName);

    Map *map = new Map;
    map->isPersistedToFile = false;
    map->setName(mapName);
    mapConstantsToMapNames->insert(map->constantName, map->name);
    mapNamesToMapConstants->insert(map->name, map->constantName);
    setNewMapHeader(map, mapIndex);
    setNewMapAttributes(map);
    getTilesets(map);
    setNewMapBlockdata(map);
    setNewMapBorder(map);
    setNewMapEvents(map);
    setNewMapConnections(map);
    map->commit();
    map->history.save();
    map_cache->insert(mapName, map);

    return map;
}

QString Project::getNewMapName() {
    // Ensure default name doesn't already exist.
    int i = 0;
    QString newMapName;
    do {
        newMapName = QString("NewMap%1").arg(++i);
    } while (mapNames->contains(newMapName));

    return newMapName;
}

QList<QStringList>* Project::parseAsm(QString text) {
    ParseUtil *parser = new ParseUtil;
    return parser->parseAsm(text);
}

QStringList Project::getLocations() {
    // TODO
    QStringList names;
    for (int i = 0; i < 88; i++) {
        names.append(QString("%1").arg(i));
    }
    return names;
}

QStringList Project::getVisibilities() {
    // TODO
    QStringList names;
    for (int i = 0; i < 16; i++) {
        names.append(QString("%1").arg(i));
    }
    return names;
}

QStringList Project::getWeathers() {
    // TODO
    QStringList names;
    for (int i = 0; i < 16; i++) {
        names.append(QString("%1").arg(i));
    }
    return names;
}

QStringList Project::getMapTypes() {
    // TODO
    QStringList names;
    for (int i = 0; i < 16; i++) {
        names.append(QString("%1").arg(i));
    }
    return names;
}

QStringList Project::getBattleScenes() {
    // TODO
    QStringList names;
    for (int i = 0; i < 16; i++) {
        names.append(QString("%1").arg(i));
    }
    return names;
}

void Project::readItemNames() {
    QString filepath = root + "/include/constants/items.h";
    QStringList prefixes = (QStringList() << "ITEM_");
    readCDefinesSorted(filepath, prefixes, itemNames);
}

void Project::readFlagNames() {
    QString filepath = root + "/include/constants/flags.h";
    QStringList prefixes = (QStringList() << "FLAG_");
    readCDefinesSorted(filepath, prefixes, flagNames);
}

void Project::readVarNames() {
    QString filepath = root + "/include/constants/vars.h";
    QStringList prefixes = (QStringList() << "VAR_");
    readCDefinesSorted(filepath, prefixes, varNames);
}

void Project::readCDefinesSorted(QString filepath, QStringList prefixes, QStringList* definesToSet) {
    QString text = readTextFile(filepath);
    if (!text.isNull()) {
        QMap<QString, int> defines = readCDefines(text, prefixes);

        // The defines should to be sorted by their underlying value, not alphabetically.
        // Reverse the map and read out the resulting keys in order.
        QMultiMap<int, QString> definesInverse;
        for (QString defineName : defines.keys()) {
            definesInverse.insert(defines[defineName], defineName);
        }
        *definesToSet = definesInverse.values();
    }
}

void Project::readMapsWithConnections() {
    QString path = root + "/data/maps/connections.inc";
    QString text = readTextFile(path);
    if (text.isNull()) {
        return;
    }

    mapsWithConnections.clear();
    QRegularExpression re("data\\/maps\\/(?<mapName>\\w+)\\/connections.inc");
    QList<QStringList>* includes = parseAsm(text);
    for (QStringList values : *includes) {
        if (values.length() != 2)
            continue;

        QRegularExpressionMatch match = re.match(values.value(1));
        if (match.hasMatch()) {
            QString mapName = match.captured("mapName");
            mapsWithConnections.append(mapName);
        }
    }
}

void Project::saveMapsWithConnections() {
    QString path = root + "/data/maps/connections.inc";
    QString text = "";
    for (QString mapName : mapsWithConnections) {
        if (mapNamesToMapConstants->contains(mapName)) {
            text += QString("\t.include \"data/maps/%1/connections.inc\"\n").arg(mapName);
        } else {
            qDebug() << QString("Failed to write connection include. %1 not a valid map name").arg(mapName);
        }
    }
    saveTextFile(path, text);
}

QStringList Project::getSongNames() {
    QStringList names;
    QString text = readTextFile(root + "/include/constants/songs.h");
    if (!text.isNull()) {
        QStringList songDefinePrefixes;
        songDefinePrefixes << "SE_" << "BGM_";
        QMap<QString, int> songDefines = readCDefines(text, songDefinePrefixes);
        names = songDefines.keys();
    }
    return names;
}

QMap<QString, int> Project::getEventObjGfxConstants() {
    QMap<QString, int> constants;
    QString text = readTextFile(root + "/include/constants/event_objects.h");
    if (!text.isNull()) {
        QStringList eventObjGfxPrefixes;
        eventObjGfxPrefixes << "EVENT_OBJ_GFX_";
        constants = readCDefines(text, eventObjGfxPrefixes);
    }
    return constants;
}

QString Project::fixGraphicPath(QString path) {
    path = path.replace(QRegExp("\\.lz$"), "");
    path = path.replace(QRegExp("\\.[1248]bpp$"), ".png");
    return path;
}

void Project::loadEventPixmaps(QList<Event*> objects) {
    bool needs_update = false;
    for (Event *object : objects) {
        if (object->pixmap.isNull()) {
            needs_update = true;
            break;
        }
    }
    if (!needs_update) {
        return;
    }

    QMap<QString, int> constants = getEventObjGfxConstants();

    QString pointers_text = readTextFile(root + "/src/data/field_event_obj/event_object_graphics_info_pointers.h");
    QString info_text = readTextFile(root + "/src/data/field_event_obj/event_object_graphics_info.h");
    QString pic_text = readTextFile(root + "/src/data/field_event_obj/event_object_pic_tables.h");
    QString assets_text = readTextFile(root + "/src/data/field_event_obj/event_object_graphics.h");

    QStringList pointers = readCArray(pointers_text, "gEventObjectGraphicsInfoPointers");

    for (Event *object : objects) {
        if (!object->pixmap.isNull()) {
            continue;
        }
        QString event_type = object->get("event_type");
        if (event_type == "object") {
            object->pixmap = QPixmap(":/images/Entities_16x16.png").copy(0, 0, 16, 16);
        } else if (event_type == "warp") {
            object->pixmap = QPixmap(":/images/Entities_16x16.png").copy(16, 0, 16, 16);
        } else if (event_type == "trap" || event_type == "trap_weather") {
            object->pixmap = QPixmap(":/images/Entities_16x16.png").copy(32, 0, 16, 16);
        } else if (event_type == "sign" || event_type == "event_hidden_item" || event_type == "event_secret_base") {
            object->pixmap = QPixmap(":/images/Entities_16x16.png").copy(48, 0, 16, 16);
        }

        if (event_type == "object") {
            int sprite_id = constants.value(object->get("sprite"));

            QString info_label = pointers.value(sprite_id).replace("&", "");
            QString pic_label = readCArray(info_text, info_label).value(14);
            QString gfx_label = readCArray(pic_text, pic_label).value(0);
            gfx_label = gfx_label.section(QRegExp("[\\(\\)]"), 1, 1);
            QString path = readCIncbin(assets_text, gfx_label);

            if (!path.isNull()) {
                path = fixGraphicPath(path);
                QPixmap pixmap(root + "/" + path);
                if (!pixmap.isNull()) {
                    object->pixmap = pixmap;
                }
            }

        }
    }

}

void Project::saveMapEvents(Map *map) {
    QString path = root + QString("/data/maps/events/%1.inc").arg(map->name);
    QString text = "";

    if (map->events["object"].length() > 0) {
        text += QString("%1::\n").arg(map->object_events_label);
        for (int i = 0; i < map->events["object"].length(); i++) {
            Event *object_event = map->events["object"].value(i);
            int radius_x = object_event->getInt("radius_x");
            int radius_y = object_event->getInt("radius_y");
            uint16_t x = object_event->getInt("x");
            uint16_t y = object_event->getInt("y");

            text += QString("\tobject_event %1").arg(i + 1);
            text += QString(", %1").arg(object_event->get("sprite"));
            text += QString(", %1").arg(object_event->get("replacement"));
            text += QString(", %1").arg(x);
            text += QString(", %1").arg(y);
            text += QString(", %1").arg(object_event->get("elevation"));
            text += QString(", %1").arg(object_event->get("behavior"));
            text += QString(", %1").arg(radius_x);
            text += QString(", %1").arg(radius_y);
            text += QString(", %1").arg(object_event->get("property"));
            text += QString(", %1").arg(object_event->get("sight_radius"));
            text += QString(", %1").arg(object_event->get("script_label"));
            text += QString(", %1").arg(object_event->get("event_flag"));
            text += "\n";
        }
        text += "\n";
    }

    if (map->events["warp"].length() > 0) {
        text += QString("%1::\n").arg(map->warps_label);
        for (Event *warp : map->events["warp"]) {
            text += QString("\twarp_def %1").arg(warp->get("x"));
            text += QString(", %1").arg(warp->get("y"));
            text += QString(", %1").arg(warp->get("elevation"));
            text += QString(", %1").arg(warp->get("destination_warp"));
            text += QString(", %1").arg(mapNamesToMapConstants->value(warp->get("destination_map_name")));
            text += "\n";
        }
        text += "\n";
    }

    if (map->events["trap"].length() + map->events["trap_weather"].length() > 0) {
        text += QString("%1::\n").arg(map->coord_events_label);
        for (Event *coords : map->events["trap"]) {
            text += QString("\tcoord_event %1").arg(coords->get("x"));
            text += QString(", %1").arg(coords->get("y"));
            text += QString(", %1").arg(coords->get("elevation"));
            text += QString(", 0");
            text += QString(", %1").arg(coords->get("script_var"));
            text += QString(", %1").arg(coords->get("script_var_value"));
            text += QString(", 0");
            text += QString(", %1").arg(coords->get("script_label"));
            text += "\n";
        }
        for (Event *coords : map->events["trap_weather"]) {
            text += QString("\tcoord_weather_event %1").arg(coords->get("x"));
            text += QString(", %1").arg(coords->get("y"));
            text += QString(", %1").arg(coords->get("elevation"));
            text += QString(", %1").arg(coords->get("weather"));
            text += "\n";
        }
        text += "\n";
    }

    if (map->events["sign"].length() +
        map->events["event_hidden_item"].length() +
        map->events["event_secret_base"].length() > 0)
    {
        text += QString("%1::\n").arg(map->bg_events_label);
        for (Event *sign : map->events["sign"]) {
            text += QString("\tbg_event %1").arg(sign->get("x"));
            text += QString(", %1").arg(sign->get("y"));
            text += QString(", %1").arg(sign->get("elevation"));
            text += QString(", %1").arg(sign->get("type"));
            text += QString(", 0");
            text += QString(", %1").arg(sign->get("script_label"));
            text += "\n";
        }
        for (Event *item : map->events["event_hidden_item"]) {
            text += QString("\tbg_hidden_item_event %1").arg(item->get("x"));
            text += QString(", %1").arg(item->get("y"));
            text += QString(", %1").arg(item->get("elevation"));
            text += QString(", %1").arg(item->get("item"));
            text += QString(", %1").arg(item->get("flag"));
            text += "\n";
        }
        for (Event *item : map->events["event_secret_base"]) {
            text += QString("\tbg_secret_base_event %1").arg(item->get("x"));
            text += QString(", %1").arg(item->get("y"));
            text += QString(", %1").arg(item->get("elevation"));
            text += QString(", %1").arg(item->get("secret_base_map"));
            text += "\n";
        }
        text += "\n";
    }

    text += QString("%1::\n").arg(map->events_label);
    text += QString("\tmap_events %1, %2, %3, %4\n")
            .arg(map->object_events_label)
            .arg(map->warps_label)
            .arg(map->coord_events_label)
            .arg(map->bg_events_label);

    saveTextFile(path, text);
}

void Project::readMapEvents(Map *map) {
    if (!map->isPersistedToFile) {
        return;
    }

    // lazy
    QString path = root + QString("/data/maps/events/%1.inc").arg(map->name);
    QString text = readTextFile(path);
    if (text.isNull()) {
        return;
    }

    QStringList *labels = getLabelValues(parseAsm(text), map->events_label);
    map->object_events_label = labels->value(0);
    map->warps_label = labels->value(1);
    map->coord_events_label = labels->value(2);
    map->bg_events_label = labels->value(3);

    QList<QStringList> *object_events = getLabelMacros(parseAsm(text), map->object_events_label);
    map->events["object"].clear();
    for (QStringList command : *object_events) {
        if (command.value(0) == "object_event") {
            Event *object = new Event;
            object->put("map_name", map->name);
            int i = 2;
            object->put("sprite", command.value(i++));
            object->put("replacement", command.value(i++));
            object->put("x", command.value(i++).toInt(nullptr, 0));
            object->put("y", command.value(i++).toInt(nullptr, 0));
            object->put("elevation", command.value(i++));
            object->put("behavior", command.value(i++));
            object->put("radius_x", command.value(i++).toInt(nullptr, 0));
            object->put("radius_y", command.value(i++).toInt(nullptr, 0));
            object->put("property", command.value(i++));
            object->put("sight_radius", command.value(i++));
            object->put("script_label", command.value(i++));
            object->put("event_flag", command.value(i++));

            object->put("event_type", "object");
            map->events["object"].append(object);
        }
    }

    QList<QStringList> *warps = getLabelMacros(parseAsm(text), map->warps_label);
    map->events["warp"].clear();
    for (QStringList command : *warps) {
        if (command.value(0) == "warp_def") {
            Event *warp = new Event;
            warp->put("map_name", map->name);
            int i = 1;
            warp->put("x", command.value(i++));
            warp->put("y", command.value(i++));
            warp->put("elevation", command.value(i++));
            warp->put("destination_warp", command.value(i++));

            // Ensure the warp destination map constant is valid before adding it to the warps.
            QString mapConstant = command.value(i++);
            if (mapConstantsToMapNames->contains(mapConstant)) {
                warp->put("destination_map_name", mapConstantsToMapNames->value(mapConstant));
                warp->put("event_type", "warp");
                map->events["warp"].append(warp);
            } else {
                qDebug() << QString("Destination map constant '%1' is invalid for warp").arg(mapConstant);
            }
        }
    }

    QList<QStringList> *coords = getLabelMacros(parseAsm(text), map->coord_events_label);
    map->events["trap"].clear();
    map->events["trap_weather"].clear();
    for (QStringList command : *coords) {
        if (command.value(0) == "coord_event") {
            Event *coord = new Event;
            coord->put("map_name", map->name);
            bool old_macro = false;
            if (command.length() >= 9) {
                command.removeAt(7);
                command.removeAt(4);
                old_macro = true;
            }
            int i = 1;
            coord->put("x", command.value(i++));
            coord->put("y", command.value(i++));
            coord->put("elevation", command.value(i++));
            coord->put("script_var", command.value(i++));
            coord->put("script_var_value", command.value(i++));
            coord->put("script_label", command.value(i++));
            //coord_unknown3
            //coord_unknown4

            coord->put("event_type", "trap");
            map->events["trap"].append(coord);
        } else if (command.value(0) == "coord_weather_event") {
            Event *coord = new Event;
            coord->put("map_name", map->name);
            int i = 1;
            coord->put("x", command.value(i++));
            coord->put("y", command.value(i++));
            coord->put("elevation", command.value(i++));
            coord->put("weather", command.value(i++));
            coord->put("event_type", "trap_weather");
            map->events["trap_weather"].append(coord);
        }
    }

    QList<QStringList> *bgs = getLabelMacros(parseAsm(text), map->bg_events_label);
    map->events["sign"].clear();
    map->events["event_hidden_item"].clear();
    map->events["event_secret_base"].clear();
    for (QStringList command : *bgs) {
        if (command.value(0) == "bg_event") {
            Event *bg = new Event;
            bg->put("map_name", map->name);
            int i = 1;
            bg->put("x", command.value(i++));
            bg->put("y", command.value(i++));
            bg->put("elevation", command.value(i++));
            bg->put("type", command.value(i++));
            i++;
            bg->put("script_label", command.value(i++));
            //sign_unknown7
            bg->put("event_type", "sign");
            map->events["sign"].append(bg);
        } else if (command.value(0) == "bg_hidden_item_event") {
            Event *bg = new Event;
            bg->put("map_name", map->name);
            int i = 1;
            bg->put("x", command.value(i++));
            bg->put("y", command.value(i++));
            bg->put("elevation", command.value(i++));
            bg->put("item", command.value(i++));
            bg->put("flag", command.value(i++));
            bg->put("event_type", "event_hidden_item");
            map->events["event_hidden_item"].append(bg);
        } else if (command.value(0) == "bg_secret_base_event") {
            Event *bg = new Event;
            bg->put("map_name", map->name);
            int i = 1;
            bg->put("x", command.value(i++));
            bg->put("y", command.value(i++));
            bg->put("elevation", command.value(i++));
            bg->put("secret_base_map", command.value(i++));
            bg->put("event_type", "event_secret_base");
            map->events["event_secret_base"].append(bg);
        }
    }
}

void Project::setNewMapEvents(Map *map) {
    map->object_events_label = "0x0";
    map->warps_label = "0x0";
    map->coord_events_label = "0x0";
    map->bg_events_label = "0x0";
    map->events["object"].clear();
    map->events["warp"].clear();
    map->events["trap"].clear();
    map->events["trap_weather"].clear();
    map->events["sign"].clear();
    map->events["event_hidden_item"].clear();
    map->events["event_secret_base"].clear();
}

QStringList Project::readCArray(QString text, QString label) {
    QStringList list;

    if (label.isNull()) {
        return list;
    }

    QRegExp *re = new QRegExp(QString("\\b%1\\b\\s*\\[?\\s*\\]?\\s*=\\s*\\{([^\\}]*)\\}").arg(label));
    int pos = re->indexIn(text);
    if (pos != -1) {
        QString body = re->cap(1);
        body = body.replace(QRegExp("\\s*"), "");
        list = body.split(',');
        /*
        QRegExp *inner = new QRegExp("&?\\b([A-Za-z0-9_\\(\\)]*)\\b,");
        int pos = 0;
        while ((pos = inner->indexIn(body, pos)) != -1) {
            list << inner->cap(1);
            pos += inner->matchedLength();
        }
        */
    }

    return list;
}

QString Project::readCIncbin(QString text, QString label) {
    QString path;

    if (label.isNull()) {
        return path;
    }

    QRegExp *re = new QRegExp(QString(
        "\\b%1\\b"
        "\\s*\\[?\\s*\\]?\\s*=\\s*"
        "INCBIN_[US][0-9][0-9]?"
        "\\(\"([^\"]*)\"\\)").arg(label));

    int pos = re->indexIn(text);
    if (pos != -1) {
        path = re->cap(1);
    }

    return path;
}

QMap<QString, int> Project::readCDefines(QString text, QStringList prefixes) {
    ParseUtil parser;
    QMap<QString, int> allDefines;
    QMap<QString, int> filteredDefines;
    QRegularExpression re("#define\\s+(?<defineName>\\w+)[^\\S\\n]+(?<defineValue>.+)");
    QRegularExpressionMatchIterator iter = re.globalMatch(text);
    while (iter.hasNext()) {
        QRegularExpressionMatch match = iter.next();
        QString name = match.captured("defineName");
        QString expression = match.captured("defineValue");
        expression.replace(QRegularExpression("//.*"), "");
        int value = parser.evaluateDefine(expression, &allDefines);
        allDefines.insert(name, value);
        for (QString prefix : prefixes) {
            if (name.startsWith(prefix)) {
                filteredDefines.insert(name, value);
            }
        }
    }
    return filteredDefines;
}
