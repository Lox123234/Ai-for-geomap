#pragma once

class AppLog;
class MapService;
class ProcessingService;
class ProjectManager;

// Feeds the operations journal from what the services report: project status and errors,
// job state changes (once per change), engine environment, installation, map errors and
// exports. Nothing is added that a service did not report.
void connectJournal(AppLog *log, ProjectManager *projects, ProcessingService *processing, MapService *map);
